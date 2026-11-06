// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Core/Protocol/Sync/test_theme_switch.cpp — a theme switch reaches the
// program running on the host.
//
// A remote application picks its own colours by asking the terminal for the
// scheme (DEC mode 2031 / DSR 996), and the terminal it asks is the HOST's. A
// local frontend writes the palette and calls rethemeDefaults, which answers a
// subscriber; a remote one had no frame to say the same thing, so switching a
// client to light left every program on that session drawing for dark. The
// symptom is not a terminal that looks wrong — it looks exactly as asked — but
// an application whose own colours no longer suit it, which reads as the
// application's bug.
//
// The light/dark verdict is derived from the background's luminance rather than
// carried as a flag, so these cases assert the derivation too: a palette is
// pushed and the SCHEME the program is told must follow from it.
//

#include "Assertions.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Messages.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/RethemeDefaults.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// Whether the program asked to be told about scheme changes at all.
enum class Subscription {
    Absent,
    Present
};

// A palette whose every slot is distinguishable, so a codec that drops or
// transposes one is caught rather than passing on values that happen to match.
Ubiquitty::TermPalette distinctPalette(uint16_t seed)
{
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    for (int i = 0; i < 256; ++i) {
        palette.indexed[i].red = static_cast<uint16_t>(seed + i);
        palette.indexed[i].green = static_cast<uint16_t>(seed + i + 1000);
        palette.indexed[i].blue = static_cast<uint16_t>(seed + i + 2000);
    }
    palette.defaultForeground = { static_cast<uint16_t>(seed + 1), static_cast<uint16_t>(seed + 2), static_cast<uint16_t>(seed + 3) };
    palette.cursor = { static_cast<uint16_t>(seed + 7), static_cast<uint16_t>(seed + 8), static_cast<uint16_t>(seed + 9) };
    for (std::size_t i = 0; i < Ubiquitty::specialCount; ++i) {
        palette.special[i] = { static_cast<uint16_t>(seed + i + 10), static_cast<uint16_t>(seed + i + 20), static_cast<uint16_t>(seed + i + 30) };
        palette.specialSet[i] = (i % 2) == 0;
    }
    return palette;
}

bool samePalette(const Ubiquitty::TermPalette& a, const Ubiquitty::TermPalette& b)
{
    for (int i = 0; i < 256; ++i) {
        if (a.indexed[i].red != b.indexed[i].red || a.indexed[i].green != b.indexed[i].green || a.indexed[i].blue != b.indexed[i].blue)
            return false;
    }
    if (a.defaultForeground.red != b.defaultForeground.red || a.defaultBackground.red != b.defaultBackground.red)
        return false;
    if (a.cursor.red != b.cursor.red || a.cursor.green != b.cursor.green || a.cursor.blue != b.cursor.blue)
        return false;
    for (std::size_t i = 0; i < Ubiquitty::specialCount; ++i) {
        if (a.specialSet[i] != b.specialSet[i])
            return false;
        if (a.special[i].red != b.special[i].red || a.special[i].green != b.special[i].green)
            return false;
    }
    return true;
}

// The bytes the host would hand the PTY after applying a pushed palette — the
// whole path, from the frame a client sends to what the program reads.
std::string replyAfterPush(uint16_t backgroundChannel, Subscription subscription)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    if (subscription == Subscription::Present)
        UbiquittyTest::feed(t, "\x1b[?2031h");
    t.outputLength = 0;
    const std::uint64_t epochBefore = t.paletteEpoch;

    Ubiquitty::Protocol::Sync::ThemePalette theme;
    theme.live = distinctPalette(0x100);
    theme.live.defaultBackground = { backgroundChannel, backgroundChannel, backgroundChannel };
    theme.baseline = theme.live;
    Ubiquitty::Protocol::Sync::Facts facts;
    facts.themePalette = theme;
    std::vector<uint8_t> body;
    Ubiquitty::Protocol::Sync::encode(body, facts);
    const auto decoded = Ubiquitty::Protocol::Sync::decodeFacts(body);
    CHECK(decoded.has_value());
    if (!decoded || !decoded->themePalette)
        return {};
    CHECK(samePalette(decoded->themePalette->live, theme.live));
    CHECK(samePalette(decoded->themePalette->baseline, theme.baseline));

    t.palette = decoded->themePalette->live;
    t.paletteBaseline = decoded->themePalette->baseline;
    Ubiquitty::Themes::rethemeDefaults(&t);
    // Every mirror keys its colour caches on the epoch, so an apply that leaves
    // it alone repaints nothing on any other attached client.
    CHECK(t.paletteEpoch > epochBefore);
    return { t.outputBuffer.data(), static_cast<std::size_t>(t.outputLength) };
}

} // namespace

int main()
{
    // A near-black background is dark, and a subscriber is told so.
    CHECK(replyAfterPush(0x1010, Subscription::Present) == "\x1b[?997;1n");

    // A near-white one is light. The verdict follows the colours pushed, which
    // is what makes an arbitrary or custom theme classify itself correctly
    // without carrying a separate is-dark flag to fall out of step.
    CHECK(replyAfterPush(0xF0F0, Subscription::Present) == "\x1b[?997;2n");

    // Unsubscribed, the switch still applies but says nothing: an unsolicited
    // report to a program that never asked is bytes it will read as input.
    CHECK(replyAfterPush(0xF0F0, Subscription::Absent).empty());

    // A truncated body is refused whole. Half a palette is colours nobody
    // chose, and applying it would be indistinguishable from a theme.
    {
        Ubiquitty::Protocol::Sync::Facts facts;
        facts.themePalette = { distinctPalette(0x200), distinctPalette(0x300) };
        std::vector<uint8_t> body;
        Ubiquitty::Protocol::Sync::encode(body, facts);
        for (std::size_t shortened = 1; shortened <= 8; ++shortened) {
            if (body.size() < shortened)
                break;
            CHECK(!Ubiquitty::Protocol::Sync::decodeFacts(std::span { body }.first(body.size() - shortened)).has_value());
        }
    }

    namespace Sync = Ubiquitty::Protocol::Sync;
    Sync::FrameDecoder obsolete;
    const std::array<std::uint8_t, 5> oldPaletteHeader { 0x14, 0, 0, 0, 0 };
    obsolete.append(oldPaletteHeader);
    CHECK(!obsolete.next());
    CHECK(obsolete.error() == Sync::EnvelopeError::UnknownKind);

    if (failures == 0)
        std::printf("theme_switch: Facts carries a theme to the host, and a subscriber is told which scheme it is\n");
    return failures == 0 ? 0 : 1;
}
