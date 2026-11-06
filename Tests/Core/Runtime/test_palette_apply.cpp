// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_palette_apply.cpp — a theme applied through the session is the theme
// that renders (Core/Runtime/TerminalSession.h).
//
// TermPalette's channels are 16-bit and the model converts at its edges:
// seedDefaults widens a theme byte with channel8To16, channel16To8 narrows one
// back, and the renderers divide by 257 and 65535. applyPalette takes bytes,
// because that is what a theme file and a frontend hold — so it owes the same
// widening. Writing the byte straight into the 16-bit field leaves every colour
// at 1/257 of itself, which narrows back to zero: a theme applied through the
// public API renders black, and nothing on the way says so.
//
// The round trip is the assertion rather than the stored number, because the
// stored number is exactly what a test can bless while the screen is wrong. A
// byte in must be that byte out, through the conversion the renderers use.
//
// The baseline and the notification are asserted beside it: a theme apply is
// all three or it is a different operation. Reverting with OSC 110/111/112
// after a theme change must land on the new theme, and a program that asked to
// be told the scheme moved chooses its own colours from ours.
//

#include "Harness.h"

#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/Themes/Themes.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace Rt = Ubiquitty::Runtime;

// A theme's tables as a frontend holds them: one byte per channel.
struct ThemeBytes {
    std::vector<std::uint8_t> indexed = std::vector<std::uint8_t>(768, 0);
    std::vector<std::uint8_t> defaults = std::vector<std::uint8_t>(9, 0);
};

ThemeBytes distinctTheme()
{
    ThemeBytes bytes;
    // Index 1, and the three defaults, at values that survive nothing by luck:
    // each differs from its neighbours and none is 0 or 255.
    bytes.indexed[3] = 200;
    bytes.indexed[4] = 150;
    bytes.indexed[5] = 100;
    bytes.defaults = { 250, 240, 230, 20, 25, 30, 190, 180, 170 };
    return bytes;
}

// Whether `call` returns while this thread holds the model lock, the way a
// flood's parse bite holds it. The worker is joined after the lock is released
// either way, so a call that waits for the lock fails the check rather than
// hanging the run.
template<typename Call>
bool returnsWhileTheModelLockIsHeld(Rt::TerminalSession& session, Call call)
{
    std::atomic<bool> hasReturned { false };
    session.lockForRead();
    std::thread worker([&] {
        call();
        hasReturned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!hasReturned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool didReturn = hasReturned.load(std::memory_order_acquire);
    session.unlockForRead();
    worker.join();
    return didReturn;
}

// Publishes the model's state the way a drain does, with one printable byte.
void publishByDrain(Rt::TerminalSession& session)
{
    const std::string byte = "x";
    session.feedBytes(Rt::ByteSpan(byte.data(), byte.size()));
}

} // namespace

TEST(aThemeByteSurvivesTheApplyAndNarrowsBackToItself)
{
    Rt::TerminalSession* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    const ThemeBytes theme = distinctTheme();
    CHECK(session->applyPalette(Rt::AlphaSpan(theme.indexed), Rt::AlphaSpan(theme.defaults)));

    session->lockForRead();
    Ubiquitty::Terminal* terminal = session->terminal();
    CHECK(terminal != nullptr);
    if (terminal) {
        // The renderers' own conversion. A palette that does not survive it is
        // one that draws a colour nobody chose.
        CHECK(Ubiquitty::channel16To8(terminal->palette.indexed[1].red) == 200);
        CHECK(Ubiquitty::channel16To8(terminal->palette.indexed[1].green) == 150);
        CHECK(Ubiquitty::channel16To8(terminal->palette.indexed[1].blue) == 100);
        CHECK(Ubiquitty::channel16To8(terminal->palette.defaultForeground.red) == 250);
        CHECK(Ubiquitty::channel16To8(terminal->palette.defaultBackground.red) == 20);
        CHECK(Ubiquitty::channel16To8(terminal->palette.cursor.red) == 190);

        // The baseline moves with the theme: an OSC 110/111/112 reset after a
        // theme change lands on the theme in force, not the one it replaced.
        CHECK(terminal->paletteBaseline.defaultBackground.red == terminal->palette.defaultBackground.red);
        CHECK(terminal->paletteBaseline.indexed[1].red == terminal->palette.indexed[1].red);
    }
    session->unlockForRead();

    releaseTerminalSession(session);
}

TEST(aThemeApplyTellsAProgramThatAskedAboutTheScheme)
{
    Rt::TerminalSession* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    // The program subscribes (DEC mode 2031), then the theme moves to a light
    // background. What it must read is the scheme, not the colours.
    const std::string subscribe = "\033[?2031h";
    session->feedBytes(Rt::ByteSpan(subscribe.data(), subscribe.size()));

    session->lockForRead();
    if (Ubiquitty::Terminal* terminal = session->terminal())
        terminal->outputLength = 0;
    session->unlockForRead();

    ThemeBytes light = distinctTheme();
    light.defaults[3] = 0xF0; // a light background
    light.defaults[4] = 0xF0;
    light.defaults[5] = 0xF0;
    CHECK(session->applyPalette(Rt::AlphaSpan(light.indexed), Rt::AlphaSpan(light.defaults)));

    std::string reply;
    session->lockForRead();
    if (Ubiquitty::Terminal* terminal = session->terminal())
        reply.assign(terminal->outputBuffer.data(), static_cast<std::size_t>(terminal->outputLength));
    session->unlockForRead();
    CHECK(reply == "\033[?997;2n");

    releaseTerminalSession(session);
}

/// A re-seed from the configuration is the other way a theme reaches a
/// terminal — the path the Swift window shell takes when the system appearance
/// flips or the config is reloaded — and it owes a subscriber the same sentence
/// the direct apply does.
///
/// Asserted separately because the two paths write the palette differently: one
/// takes bytes from a frontend, the other re-reads the whole configuration
/// ladder. A program that chose its colours from ours cannot tell them apart
/// and should not have to.
TEST(aThemeReseedTellsAProgramThatAskedAboutTheScheme)
{
    Rt::TerminalSession* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    // Find a light theme to re-seed to, so the scheme actually moves off the
    // dark default and the reply says which way.
    int lightTheme = -1;
    for (int index = 0; index < Ubiquitty::Themes::count(); ++index) {
        if (!Ubiquitty::Themes::isDark(index)) {
            lightTheme = index;
            break;
        }
    }
    CHECK(lightTheme >= 0);
    const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(lightTheme);
    CHECK(theme != nullptr);
    if (!theme) {
        releaseTerminalSession(session);
        return;
    }

    const std::string subscribe = "\033[?2031h";
    session->feedBytes(Rt::ByteSpan(subscribe.data(), subscribe.size()));
    session->lockForRead();
    if (Ubiquitty::Terminal* terminal = session->terminal())
        terminal->outputLength = 0;
    session->unlockForRead();

    Ubiquitty::Preferences::Values config;
    config.themeName = theme->name;
    CHECK(session->reseedLaunchTheme(config));

    std::string reply;
    session->lockForRead();
    if (Ubiquitty::Terminal* terminal = session->terminal())
        reply.assign(terminal->outputBuffer.data(), static_cast<std::size_t>(terminal->outputLength));
    session->unlockForRead();
    CHECK(reply == "\033[?997;2n");

    releaseTerminalSession(session);
}

TEST(namedThemeApplyMatchesLaunchAdaptationAndRejectsUnknownTheme)
{
    Rt::TerminalSession* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    auto& adaptation = Ubiquitty::Accessibility::paletteAdaptation();
    const auto savedAdaptation = adaptation;
    adaptation.contrastStrength = 1;
    Ubiquitty::Terminal reference;
    CHECK(Ubiquitty::Themes::applyThemeToTerminal(reference, 0));
    CHECK(session->applyTheme(0));
    session->lockForRead();
    auto* terminal = session->terminal();
    CHECK(terminal != nullptr);
    if (terminal) {
        for (int index = 0; index < 256; ++index) {
            CHECK(terminal->palette.indexed[index].red == reference.palette.indexed[index].red);
            CHECK(terminal->palette.indexed[index].green == reference.palette.indexed[index].green);
            CHECK(terminal->palette.indexed[index].blue == reference.palette.indexed[index].blue);
        }
        CHECK(terminal->paletteBaseline.defaultBackground.red == reference.paletteBaseline.defaultBackground.red);
    }
    session->unlockForRead();
    CHECK(!session->applyTheme(-1));
    CHECK(!session->applyTheme(Ubiquitty::Themes::count()));
    adaptation = savedAdaptation;
    releaseTerminalSession(session);
}

TEST(backgroundOverridePreservesThemeIdentityAndResetBaseline)
{
    Rt::TerminalSession* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->applyTheme(0));
    CHECK(session->currentThemeIndex() == 0);
    session->lockForRead();
    const auto baseline = session->terminal()->paletteBaseline.defaultBackground;
    session->unlockForRead();
    CHECK(session->setInspectorBackground(17, 29, 41));
    CHECK(session->currentThemeIndex() == 0);
    session->lockForRead();
    CHECK(Ubiquitty::channel16To8(session->terminal()->palette.defaultBackground.red) == 17);
    CHECK(session->terminal()->paletteBaseline.defaultBackground.red == baseline.red);
    CHECK(session->terminal()->paletteBaseline.defaultBackground.green == baseline.green);
    CHECK(session->terminal()->paletteBaseline.defaultBackground.blue == baseline.blue);
    session->unlockForRead();
    const std::string reset = "\033]111\007";
    session->feedBytes(Rt::ByteSpan(reset.data(), reset.size()));
    session->lockForRead();
    CHECK(session->terminal()->palette.defaultBackground.red == baseline.red);
    CHECK(session->terminal()->palette.defaultBackground.green == baseline.green);
    CHECK(session->terminal()->palette.defaultBackground.blue == baseline.blue);
    session->unlockForRead();
    const auto custom = distinctTheme();
    CHECK(session->applyPalette(Rt::AlphaSpan(custom.indexed), Rt::AlphaSpan(custom.defaults)));
    CHECK(session->currentThemeIndex() == -1);
    releaseTerminalSession(session);
}

TEST(themeIdentityFollowsCatalogReplacementAndRegistration)
{
    auto* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    auto theme = *Ubiquitty::Themes::get(0);
    theme.name = "Inspector cache test";
    theme.defaultBackground = { 17, 31, 43, 255 };
    const int index = Ubiquitty::Themes::registerCustom(theme);
    CHECK(session->applyTheme(index));
    CHECK(session->currentThemeIndex() == index);
    CHECK(session->currentThemeIndex() == index);
    auto replacement = theme;
    replacement.defaultBackground = { 21, 35, 49, 255 };
    CHECK(Ubiquitty::Themes::replaceCustom(index, replacement));
    CHECK(session->currentThemeIndex() == -1);
    CHECK(session->currentThemeIndex() == -1);
    CHECK(Ubiquitty::Themes::replaceCustom(index, theme));
    CHECK(session->currentThemeIndex() == index);
    Ubiquitty::Themes::clearCustom();
    CHECK(session->currentThemeIndex() == -1);
    releaseTerminalSession(session);
}

TEST(backgroundResetPreservesSixteenBitBaselineAndOtherSlots)
{
    auto* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->applyTheme(0));
    CHECK(!session->canResetBackground());
    CHECK(!session->resetBackground());
    session->lockForRead();
    auto* terminal = session->terminal();
    terminal->paletteBaseline.defaultBackground = { 0x1234, 0x5678, 0x9ABC };
    terminal->palette.defaultForeground = { 0x1122, 0x3344, 0x5566 };
    terminal->palette.indexed[5] = { 0x1357, 0x2468, 0x369A };
    terminal->palette.indexedAlpha[5] = 73;
    const auto baseline = terminal->paletteBaseline;
    session->unlockForRead();
    CHECK(session->setInspectorBackground(17, 29, 41));
    CHECK(session->canResetBackground());
    CHECK(session->resetBackground());
    CHECK(!session->canResetBackground());
    CHECK(!session->resetBackground());
    session->lockForRead();
    CHECK(terminal->palette.defaultBackground.red == 0x1234);
    CHECK(terminal->palette.defaultBackground.green == 0x5678);
    CHECK(terminal->palette.defaultBackground.blue == 0x9ABC);
    CHECK(terminal->palette.defaultForeground.red == 0x1122);
    CHECK(terminal->palette.defaultForeground.green == 0x3344);
    CHECK(terminal->palette.defaultForeground.blue == 0x5566);
    CHECK(terminal->palette.indexed[5].red == 0x1357);
    CHECK(terminal->palette.indexed[5].green == 0x2468);
    CHECK(terminal->palette.indexed[5].blue == 0x369A);
    CHECK(terminal->palette.indexedAlpha[5] == 73);
    CHECK(terminal->paletteBaseline.defaultBackground.red == baseline.defaultBackground.red);
    CHECK(terminal->paletteBaseline.defaultBackground.green == baseline.defaultBackground.green);
    CHECK(terminal->paletteBaseline.defaultBackground.blue == baseline.defaultBackground.blue);
    session->unlockForRead();
    releaseTerminalSession(session);
}

UBIQUITTY_TEST_MAIN()

TEST(equalPalettesRetainDistinctThemeAppearance)
{
    auto* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    auto glass = *Ubiquitty::Themes::get(0);
    glass.name = "Glass identity test";
    glass.appearance.useGlass();
    const int index = Ubiquitty::Themes::registerCustom(glass);
    CHECK(session->applyTheme(index));
    CHECK(session->currentThemeIndex() == index);
    CHECK(session->applyTheme(0));
    CHECK(session->currentThemeIndex() == 0);
    CHECK(session->applyTheme(index));
    CHECK(session->currentThemeIndex() == index);
    releaseTerminalSession(session);
    Ubiquitty::Themes::clearCustom();
}

// The window chrome asks for the theme on every screen update. Under a flood the
// parser holds the model lock for a bite at a time, and an answer that waits
// for it parks the main thread for that bite on every ask: the frame clock
// shares that thread, so a flood the parser keeps up with still drops frames.
TEST(anUnchangedThemeIdentityIsAnsweredWhileTheParserHoldsTheModelLock)
{
    auto* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->applyTheme(0));
    publishByDrain(*session);
    CHECK(session->currentThemeIndex() == 0);
    int answer = -2;
    CHECK_MSG(returnsWhileTheModelLockIsHeld(*session, [&] { answer = session->currentThemeIndex(); }),
        "nothing moved the palette since the identity was matched, so the answer needs no lock");
    CHECK(answer == 0);
    releaseTerminalSession(session);
}

// The lock-free answer stands only while the published palette epoch does. A
// baseline written under the lock and then published is a different palette,
// and an identity that ignored the epoch would keep naming the old theme.
TEST(aPublishedBaselineWriteIsNotAnsweredFromTheOldIdentity)
{
    auto* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->applyTheme(0));
    publishByDrain(*session);
    CHECK(session->currentThemeIndex() == 0);
    session->lockForRead();
    Ubiquitty::Terminal* terminal = session->terminal();
    CHECK(terminal != nullptr);
    if (terminal) {
        terminal->paletteBaseline.defaultBackground = { 0x1234, 0x5678, 0x9ABC };
        ++terminal->paletteEpoch;
    }
    session->unlockForRead();
    publishByDrain(*session);
    CHECK(session->currentThemeIndex() == -1);
    releaseTerminalSession(session);
}

// A palette applied on the frontend's thread moves the baseline before any
// drain publishes the new epoch, so the identity matched at the published one
// has to be dropped by the apply itself.
TEST(aPaletteAppliedBetweenDrainsIsNotAnsweredFromTheOldIdentity)
{
    auto* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->applyTheme(0));
    publishByDrain(*session);
    CHECK(session->currentThemeIndex() == 0);
    const auto custom = distinctTheme();
    CHECK(session->applyPalette(Rt::AlphaSpan(custom.indexed), Rt::AlphaSpan(custom.defaults)));
    CHECK(session->currentThemeIndex() == -1);
    releaseTerminalSession(session);
}

// The frontend restates its presentation on every screen update; only an edge
// may wait for the parser.
TEST(anUnchangedSyncFramePresentationIsAcceptedWhileTheParserHoldsTheModelLock)
{
    auto* session = Rt::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    session->setSyncFramePresentation(Rt::PanePresentation::Presented);
    CHECK_MSG(returnsWhileTheModelLockIsHeld(*session, [&] { session->setSyncFramePresentation(Rt::PanePresentation::Presented); }),
        "a restatement of the standing presentation changes nothing the lock guards");
    session->setSyncFramePresentation(Rt::PanePresentation::Hidden);
    CHECK_MSG(returnsWhileTheModelLockIsHeld(*session, [&] { session->setSyncFramePresentation(Rt::PanePresentation::Hidden); }),
        "nor does a restatement of a hidden one");
    releaseTerminalSession(session);
}
