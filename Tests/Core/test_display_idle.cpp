// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/CellGrid.h"
#include "Core/Modes/DisplayIdle.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "TerminalFixture.h"
#include <cstring>

using namespace Ubiquitty;
using namespace Ubiquitty::Modes::DisplayIdle;
using UbiquittyTest::feed;

namespace {
class IdleClock final : public TerminalClock {
public:
    Ticks now() override { return Ticks(ticks); }
    std::uint32_t ticks = 100;
};
}

TEST(crtSaverBlanksAtThirtyMinutesWithoutChangingCellsOrCursor)
{
    Terminal terminal;
    resize(terminal, 2, 4);
    feed(terminal, "\033[?97h");
    feed(terminal, "ABCD");
    const auto cursor = terminal.screens.active->cursorX;
    CHECK(!noteInput(terminal, Ticks(100)));
    CHECK(!advance(terminal, Ticks(100 + crtSaveDelayTicks - 1)));
    CHECK(ticksUntilDeadline(terminal, Ticks(100 + crtSaveDelayTicks - 1)) == 1);
    CHECK(advance(terminal, Ticks(100 + crtSaveDelayTicks)));
    CHECK(terminal.screenBlanked);
    CHECK(!ticksUntilDeadline(terminal, Ticks(100 + crtSaveDelayTicks)));
    CHECK(UbiquittyTest::cell(terminal, 0, 0)->codePoint == 'A');
    CHECK(UbiquittyTest::cell(terminal, 0, 3)->codePoint == 'D');
    CHECK(terminal.screens.active->cursorX == cursor);
    CHECK(noteInput(terminal, Ticks(100 + crtSaveDelayTicks + 1)));
    CHECK(!terminal.screenBlanked);
    feed(terminal, "E");
    CHECK(UbiquittyTest::cell(terminal, 1, 0)->codePoint == 'E');
}

TEST(crtSaverAnyHostByteWakesBothParserEntryPoints)
{
    IdleClock clock;
    Terminal terminal;
    resize(terminal, 2, 4);
    terminal.tickClock = &clock;
    feed(terminal, "\033[?97h");
    for (int entry = 0; entry < 2; ++entry) {
        CHECK(!noteInput(terminal, clock.now()));
        clock.ticks += crtSaveDelayTicks;
        CHECK(advance(terminal, clock.now()));
        ++clock.ticks;
        if (entry == 0)
            processBytes(&terminal, std::string_view("\0", 1));
        else
            (void)processBytesUpToSyncClear(&terminal, std::string_view("\0", 1));
        CHECK(!terminal.screenBlanked);
        CHECK(ticksUntilDeadline(terminal, clock.now()) == crtSaveDelayTicks);
    }
    terminal.tickClock = nullptr;
}

TEST(crtSaverDisableWakesWithoutAClockAndReportsItsState)
{
    Terminal terminal;
    hardReset(terminal);
    feed(terminal, "\033[?97h");
    CHECK(!noteInput(terminal, Ticks(1)));
    CHECK(advance(terminal, Ticks(1 + crtSaveDelayTicks)));
    feed(terminal, "\033[?97l\033[?97$p");
    CHECK(!terminal.screenBlanked);
    CHECK(!terminal.modes.shouldSaveCRT);
    CHECK(!advance(terminal, Ticks(1 + 2 * crtSaveDelayTicks)));
    CHECK(!ticksUntilDeadline(terminal, Ticks(1)));
    char reply[64];
    UbiquittyTest::drain(terminal, reply);
    CHECK(std::strcmp(reply, "\033[?97;2$y") == 0);
    feed(terminal, "\033[?97h\033[?97$p");
    UbiquittyTest::drain(terminal, reply);
    CHECK(std::strcmp(reply, "\033[?97;1$y") == 0);
}

TEST(crtSaverTimerWrapsAndRestoredBlankWaitsForInput)
{
    Terminal terminal;
    resize(terminal, 2, 4);
    feed(terminal, "\033[?97h");
    constexpr std::uint32_t start = 0xfffffff0u;
    CHECK(!noteInput(terminal, Ticks(start)));
    CHECK(!advance(terminal, Ticks(start + crtSaveDelayTicks - 1)));
    CHECK(advance(terminal, Ticks(start + crtSaveDelayTicks)));
    terminal.hasDisplayActivityTick = false;
    CHECK(!advance(terminal, Ticks(1)));
    CHECK(terminal.screenBlanked);
    CHECK(noteInput(terminal, Ticks(2)));
    CHECK(!terminal.screenBlanked);
}

TEST(crtSaverIsOffUntilAProgramSetsIt)
{
    Terminal terminal;
    resize(terminal, 2, 4);
    feed(terminal, "ABCD");
    CHECK(!terminal.modes.shouldSaveCRT);
    CHECK(!noteInput(terminal, Ticks(100)));
    CHECK(!ticksUntilDeadline(terminal, Ticks(100)));
    CHECK(!advance(terminal, Ticks(100 + crtSaveDelayTicks)));
    constexpr std::uint32_t aDayLater = 100 + 48 * crtSaveDelayTicks;
    CHECK(!advance(terminal, Ticks(aDayLater)));
    CHECK(!terminal.screenBlanked);
    CHECK(!terminal.isCRTBlanked);
    CHECK(!ticksUntilDeadline(terminal, Ticks(aDayLater)));
    CHECK(UbiquittyTest::cell(terminal, 0, 0)->codePoint == 'A');
    feed(terminal, "\033[!p");
    CHECK(!terminal.modes.shouldSaveCRT);
    CHECK(!ticksUntilDeadline(terminal, Ticks(aDayLater)));
    feed(terminal, "\033[?97h");
    CHECK(terminal.modes.shouldSaveCRT);
    CHECK(ticksUntilDeadline(terminal, Ticks(aDayLater)).has_value());
    hardReset(terminal);
    CHECK(!terminal.modes.shouldSaveCRT);
    CHECK(!ticksUntilDeadline(terminal, Ticks(aDayLater)));
}

TEST(crtSaverReportsResetUntilSetAndAfterRIS)
{
    Terminal terminal;
    resize(terminal, 2, 4);
    char reply[64];
    feed(terminal, "\033[?97$p");
    UbiquittyTest::drain(terminal, reply);
    CHECK(std::strcmp(reply, "\033[?97;2$y") == 0);
    feed(terminal, "\033[?97h\033[?97$p");
    UbiquittyTest::drain(terminal, reply);
    CHECK(std::strcmp(reply, "\033[?97;1$y") == 0);
    hardReset(terminal);
    feed(terminal, "\033[?97$p");
    UbiquittyTest::drain(terminal, reply);
    CHECK(std::strcmp(reply, "\033[?97;2$y") == 0);
}

TEST(crtSaverDoesNotWakeAnotherDisplayPowerBlank)
{
    Terminal terminal;
    terminal.screenBlanked = true;
    CHECK(!noteInput(terminal, Ticks(10)));
    CHECK(terminal.screenBlanked);
}

TEST(blankDisplayCoversTheViewportWithoutAnAtlas)
{
    using namespace Rendering;
    BuildInputs inputs;
    inputs.isScreenBlanked = true;
    inputs.viewportSizeInPixels = { 640, 480 };
    OverlayInstance cover {};
    InstanceBuffers output;
    output.overlays = &cover;
    output.overlayCapacity = 1;
    const auto counts = buildCellInstances(inputs, {}, output);
    CHECK(counts.overlays == 1);
    CHECK(counts.cells == 0);
    CHECK(counts.decorations == 0);
    CHECK(cover.topLeftInPixels.x == 0 && cover.topLeftInPixels.y == 0);
    CHECK(cover.bottomRightInPixels.x == 640 && cover.bottomRightInPixels.y == 480);
    CHECK(cover.color.x == 0 && cover.color.y == 0 && cover.color.z == 0 && cover.color.w == 1);
    CHECK(buildCellInstances(inputs, {}, {}).overlays == 1);
}

UBIQUITTY_TEST_MAIN()
