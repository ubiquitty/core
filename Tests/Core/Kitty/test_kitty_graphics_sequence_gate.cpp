// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_kitty_graphics_sequence_gate — kitty graphics under `[security]
// apc_allow` / `apc_deny`.
//
// The sequence gate keys an APC on its first byte, `G` for kitty graphics. A
// denied APC is consumed whole like a denied DCS: nothing is stored or placed,
// nothing is answered, and a chunked transfer never opens. An allowed one, and
// the default empty gate, run the protocol as an ungated terminal does.

#include "Harness.h"

#include "Core/Image/TerminalImage.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "KittyGraphicsFixture.h"
#include "TerminalFixture.h"

#include <cstdint>
#include <string>

namespace {

using UbiquittyTest::drain;
using UbiquittyTest::Kitty::apc;
using UbiquittyTest::Kitty::base64;
using UbiquittyTest::Kitty::feed;
using UbiquittyTest::Kitty::makeRGBA;

constexpr std::uint8_t kittyIntroducer = 'G';

int placementCount(const Ubiquitty::Terminal& t)
{
    int count = 0;
    for (const Ubiquitty::Image::TerminalImagePlacement& placement : t.images.placements) {
        if (placement.used)
            ++count;
    }
    return count;
}

int storedCount(const Ubiquitty::Terminal& t)
{
    int count = 0;
    for (const Ubiquitty::Image::TerminalImage& image : t.images.stored) {
        if (image.used)
            ++count;
    }
    return count;
}

std::string replies(Ubiquitty::Terminal& t)
{
    char outputBuffer[512];
    const int written = drain(t, outputBuffer);
    return std::string(outputBuffer, static_cast<std::size_t>(written));
}

void prepare(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, 10, 40, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
}

/// A 2x2 transmit-and-display that asks for its reply (no `q`).
std::string transmitAndDisplay()
{
    return apc("a=T,f=32,s=2,v=2,i=7,c=2,r=1,C=1", base64(makeRGBA(2, 2)));
}

} // namespace

TEST(aDeniedKittyTransmitDrawsNothingAndAnswersNothing)
{
    Ubiquitty::Terminal t;
    prepare(t);
    const std::uint8_t deny[] = { kittyIntroducer };
    CHECK(Ubiquitty::OSC::setAPCDenyList(t, deny));
    CHECK(!Ubiquitty::OSC::apcAllowed(t, kittyIntroducer));

    feed(t, transmitAndDisplay());
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(storedCount(t) == 0, "a denied transmit stores no image");
    CHECK_MSG(placementCount(t) == 0, "and places none");
    CHECK_MSG(replies(t).empty(), "and is consumed with no reply, as a denied DCS is");

    // A query is the reply-only command: denied, it is not answered either.
    feed(t, apc("a=q,i=31,s=1,v=1,f=32", base64(makeRGBA(1, 1))));
    CHECK_MSG(replies(t).empty(), "a denied query is not answered");

    // The text after the denied APC still reaches the grid: the APC was
    // consumed whole, not left to swallow what follows.
    feed(t, "X");
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == 'X');
}

TEST(aDeniedChunkedTransferNeverOpens)
{
    Ubiquitty::Terminal t;
    prepare(t);
    const std::uint8_t deny[] = { kittyIntroducer };
    CHECK(Ubiquitty::OSC::setAPCDenyList(t, deny));

    const std::string whole = base64(makeRGBA(4, 4));
    const std::size_t half = whole.size() / 2;
    feed(t, apc("a=T,f=32,s=4,v=4,i=9,c=1,r=1,C=1,m=1", whole.substr(0, half)));
    CHECK_MSG(!t.images.kittyChunkActive, "a denied opener leaves no transfer open");
    feed(t, apc("m=0", whole.substr(half)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(storedCount(t) == 0);
    CHECK(placementCount(t) == 0);
    CHECK(replies(t).empty());
}

TEST(aTransferOpenWhenTheGateClosesIsAbandoned)
{
    // The gate is reinstalled whole on a configuration reload. A transfer whose
    // first block ran under the old policy cannot finish under the new one, and
    // must not finish later, once the gate reopens, with the denied blocks
    // missing from its middle.
    Ubiquitty::Terminal t;
    prepare(t);
    const std::string whole = base64(makeRGBA(4, 4));
    const std::size_t third = whole.size() / 3;
    feed(t, apc("a=T,f=32,s=4,v=4,i=9,c=1,r=1,C=1,q=2,m=1", whole.substr(0, third)));
    CHECK(t.images.kittyChunkActive);

    const std::uint8_t deny[] = { kittyIntroducer };
    CHECK(Ubiquitty::OSC::setAPCDenyList(t, deny));
    feed(t, apc("m=1", whole.substr(third, third)));
    CHECK_MSG(!t.images.kittyChunkActive, "a denied block abandons the open transfer");

    CHECK(Ubiquitty::OSC::setAPCDenyList(t, {}));
    feed(t, apc("m=0", whole.substr(2 * third)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(storedCount(t) == 0, "the tail alone stores nothing");
    CHECK(placementCount(t) == 0);
}

TEST(anAllowedKittyTransmitStillDraws)
{
    Ubiquitty::Terminal t;
    prepare(t);
    const std::uint8_t allow[] = { kittyIntroducer };
    CHECK(Ubiquitty::OSC::setAPCAllowList(t, allow));
    CHECK(Ubiquitty::OSC::apcAllowed(t, kittyIntroducer));

    feed(t, transmitAndDisplay());
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(storedCount(t) == 1);
    CHECK(placementCount(t) == 1);
    CHECK_MSG(replies(t).find("\033_Gi=7;OK\033\\") != std::string::npos, "an allowed transmit is answered");
}

TEST(anAllowListWithoutGDeniesKittyGraphics)
{
    // A non-empty allow list is exclusive, as it is for OSC and DCS.
    Ubiquitty::Terminal t;
    prepare(t);
    const std::uint8_t allow[] = { 'X' };
    CHECK(Ubiquitty::OSC::setAPCAllowList(t, allow));
    feed(t, transmitAndDisplay());
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(storedCount(t) == 0);
    CHECK(placementCount(t) == 0);
    CHECK(replies(t).empty());
}

TEST(theDefaultConfigurationLeavesKittyGraphicsOpen)
{
    Ubiquitty::Terminal t;
    prepare(t);
    Ubiquitty::Runtime::seedSequenceGate(t, Ubiquitty::Preferences::Values {});
    CHECK(Ubiquitty::OSC::apcAllowed(t, kittyIntroducer));

    feed(t, transmitAndDisplay());
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(storedCount(t) == 1);
    CHECK(placementCount(t) == 1);
    CHECK(replies(t).find("OK") != std::string::npos);
}

TEST(aConfiguredAPCDenyReachesTheDispatch)
{
    // The whole route: the [security] key, the fold, and the check before
    // kitty's dispatch.
    Ubiquitty::Terminal t;
    prepare(t);
    Ubiquitty::Preferences::Values config;
    config.apcDeny = "G";
    Ubiquitty::Runtime::seedSequenceGate(t, config);

    feed(t, transmitAndDisplay());
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(storedCount(t) == 0);
    CHECK(placementCount(t) == 0);
    CHECK(replies(t).empty());
}

UBIQUITTY_TEST_MAIN()
