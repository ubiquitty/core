// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Cursor/RenderState.h"
#include "Core/Preferences/CursorPreferences.h"
#include "Core/Preferences/PreferencesDefaults.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Terminfo.h"

#include <string>

using namespace Ubiquitty;

static void feed(Terminal& t, const std::string& bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

static void restoreCursorFromTerminfo(Terminal& terminal)
{
    const auto sequence = Terminfo::lookup("Se");
    REQUIRE(sequence.has_value());
    feed(terminal, std::string(*sequence));
}

static Preferences::Values blinkingBarConfiguration()
{
    Preferences::Values c;
    c.cursorShape = Preferences::CursorShape::Bar;
    c.cursorBlink = true;
    return c;
}

/// A terminal nobody applied the preferences to answers a program's
/// "restore the default cursor" with a block rather than the configured shape.
///
/// The shape is the struct default rather than the configured one, which is
/// silent: the cursor still draws. The blink is not lost with it — `cnorm`,
/// which every terminfo entry here spells `\E[?12l\E[?25h`, does not move the
/// drawn blink, so the reset's own default (blink on) stands.
TEST(testAnUnseededTerminalFallsBackToABlock)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);

    feed(t, "\033[5 q"); // a program asks for a blinking bar
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Bar);

    feed(t, "\033[?12l\033[?25h"); // terminfo cnorm
    restoreCursorFromTerminfo(t);

    // The fixture asserted first: this is the state the defect leaves behind.
    CHECK(t.cursorShapePref == 0);
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Block);
    CHECK(Cursor::isBlinking(Cursor::effectiveStyleForUserPreference(t)));
}

/// An unset `[cursor] blink` and a stated `blink = true` are one configuration.
///
/// The cnorm a curses program sends on exit is the traffic that would tell
/// them apart if the blink depended on whether the key was written. Two
/// documents meaning the same thing configure the same terminal.
TEST(testAnUnsetBlinkAndAStatedDefaultSurviveCnormAlike)
{
    Preferences::Values stated;
    stated.cursorBlink = Preferences::Defaults::cursorBlink;
    const Preferences::Values unset;
    bool isBlinking[2] = { false, false };
    const Preferences::Values* configurations[2] = { &unset, &stated };
    for (int i = 0; i < 2; ++i) {
        Terminal t;
        hardReset(t);
        resize(t, 6, 20);
        Preferences::Effective::applyCursorPreferences(t, *configurations[i]);
        feed(t, "\033[?12l\033[?25h"); // terminfo cnorm
        isBlinking[i] = Cursor::isBlinking(Cursor::effectiveStyleForUserPreference(t));
    }
    CHECK_MSG(isBlinking[0] == isBlinking[1], "an unset blink behaves exactly as the stated default");
    CHECK_MSG(isBlinking[0], "cnorm does not stop the reader's default blink");
}

/// The same program traffic over a seeded terminal returns the user's cursor.
TEST(testSeededPreferencesSurviveCnormAndSe)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    Preferences::Effective::applyCursorPreferences(t, blinkingBarConfiguration());

    feed(t, "\033[5 q");
    feed(t, "\033[?12l\033[?25h"); // cnorm must not silence a user who asked for blink
    feed(t, "\033[0 q");
    CHECK(t.cursorStyle == Cursor::Style::BlinkingBlock);
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Block);
    restoreCursorFromTerminfo(t);

    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Bar);
    CHECK(Cursor::isBlinking(Cursor::effectiveStyleForUserPreference(t)));
    CHECK(t.cursorBlinkPref);
}

/// The preferences are read through the accessors, so an unconfigured
/// configuration seeds the documented defaults rather than a second set of
/// them spelled at the call site.
TEST(testUnsetConfigurationSeedsTheDocumentedDefaults)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    const Preferences::Values unset; // nothing set at all
    Preferences::Effective::applyCursorPreferences(t, unset);

    CHECK(t.cursorShapePref == static_cast<std::uint8_t>(Preferences::Defaults::cursorShape));
    CHECK(t.cursorBlinkPref == Preferences::Defaults::cursorBlink);
    CHECK(t.cursorHoldFrames == static_cast<std::uint32_t>(Preferences::Defaults::cursorLinger));
    CHECK(t.cursorBlinkPeriodMilliseconds == static_cast<std::uint32_t>(Preferences::Defaults::cursorBlinkRate));
    // 1000 ms and the caret's own 60-tick cycle are the same period at 60 Hz,
    // which is what lets the default travel as a number instead of as a zero
    // meaning "ask Core".
    CHECK(Cursor::blinkPeriodTicks(t.cursorBlinkPeriodMilliseconds) == Cursor::Blink::caretPeriodTicks);
    // The glide travels with the blink: an unset motion seeds Off, so a caret
    // nobody configured snaps, and the duration seeds the documented default
    // rather than the zero the animator would otherwise have to interpret.
    CHECK(t.cursorMotionModePref == Preferences::Defaults::cursorMotion);
    CHECK(t.cursorMotionDurationMilliseconds == static_cast<std::uint32_t>(Preferences::Defaults::cursorMotionDuration));
}

static Preferences::Values glidingConfiguration()
{
    Preferences::Values c;
    c.cursorMotion = Preferences::CursorMotionMode::Always;
    c.cursorMotionDuration = 250;
    return c;
}

/// The configured glide reaches the terminal and outlives a hard reset.
///
/// The motion policy is display policy the way the blink is, so it rides the
/// same seed and the same RIS carry-over: a program that resets the terminal
/// must not turn the reader's glide off, any more than it may silence a blink
/// the reader asked for. Before this seed the mode and the duration lived in a
/// settings store on the renderer that no shell ever wrote, so no arm glided
/// whatever the configuration said.
TEST(testTheConfiguredGlideSeedsTheTerminalAndSurvivesRIS)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    Preferences::Effective::applyCursorPreferences(t, glidingConfiguration());
    CHECK(t.cursorMotionModePref == Preferences::CursorMotionMode::Always);
    CHECK(t.cursorMotionDurationMilliseconds == 250u);

    feed(t, "\033c"); // RIS
    CHECK_MSG(t.cursorMotionModePref == Preferences::CursorMotionMode::Always, "a reset keeps the reader's glide");
    CHECK_MSG(t.cursorMotionDurationMilliseconds == 250u, "and the duration they configured");
}

static Preferences::Values shapeConfiguration(Preferences::CursorShape shape)
{
    Preferences::Values c;
    c.cursorShape = shape;
    return c;
}

/// The configured shape governs before any program has said anything.
///
/// `testSeededPreferencesSurviveCnormAndSe` reaches the configured shape only
/// after a DECSCUSR and an `Se` have travelled through, so it cannot tell a
/// preference that governs from one that is merely restored. A user who never
/// runs a program that reshapes the cursor is the common case, and this is its
/// assertion.
TEST(testTheConfiguredShapeGovernsBeforeAnyProgramSpeaks)
{
    const struct {
        Preferences::CursorShape configured;
        Cursor::Shape expected;
    } cases[] = {
        { Preferences::CursorShape::Bar, Cursor::Shape::Bar },
        { Preferences::CursorShape::Underline, Cursor::Shape::Underline },
        { Preferences::CursorShape::Block, Cursor::Shape::Block },
    };
    for (const auto& c : cases) {
        Terminal t;
        hardReset(t);
        resize(t, 6, 20);
        Preferences::Effective::applyCursorPreferences(t, shapeConfiguration(c.configured));
        // No feed at all: this is the terminal as the user meets it.
        CHECK(t.cursorStyle == Cursor::Style::FollowUserPreference);
        CHECK(Cursor::resolve(&t, 0).shape == c.expected);
    }
}

/// The launch seed every arm's live path runs carries the shape, not only
/// the blink.
///
/// A seed that writes the blink pair by hand and leaves the shape at the
/// struct's zero draws a block for a user who configured an underline, and
/// draws it silently: every other seeded field answers correctly, and the
/// cursor is still there. So the seed is asked for the resolved shape, and
/// for the product default when the configuration is silent — the default the
/// documented preference names, not the block a bare Terminal starts on.
TEST(testTheLaunchSeedCarriesTheConfiguredShape)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    (void)Runtime::seedTerminalConfiguration(t, shapeConfiguration(Preferences::CursorShape::Underline),
        Runtime::LaunchThemePolicy::ConfiguredOnly);
    CHECK(t.cursorShapePref == static_cast<std::uint8_t>(Preferences::CursorShape::Underline));
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Underline);

    Terminal silent;
    hardReset(silent);
    resize(silent, 6, 20);
    (void)Runtime::seedTerminalConfiguration(silent, Preferences::Values {}, Runtime::LaunchThemePolicy::ConfiguredOnly);
    CHECK(silent.cursorShapePref == static_cast<std::uint8_t>(Preferences::Defaults::cursorShape));
    CHECK(silent.cursorBlinkPref == Preferences::Defaults::cursorBlink);
}

/// DECSCUSR outranks the preference while it is set, and hands it back at Se.
///
TEST(testAProgramOverridesTheConfiguredShapeAndGivesItBack)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    Preferences::Effective::applyCursorPreferences(t, shapeConfiguration(Preferences::CursorShape::Bar));
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Bar);

    feed(t, "\033[4 q"); // steady underline — an application reshape
    CHECK(t.cursorStyle == Cursor::Style::SteadyUnderline);
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Underline);

    feed(t, "\033[2 q"); // steady block, still the application's choice
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Block);

    restoreCursorFromTerminfo(t);
    CHECK(t.cursorStyle == Cursor::Style::FollowUserPreference);
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Bar);
}

/// A configured shape survives a shape-changing program that never says Se.
///
/// Programs exit without restoring, and the preference has to come back at the
/// reset rather than staying wherever the last one left it.
TEST(testAResetReturnsToTheConfiguredShapeNotToDECsBlock)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    Preferences::Effective::applyCursorPreferences(t, shapeConfiguration(Preferences::CursorShape::Underline));

    feed(t, "\033[5 q"); // a program picks a blinking bar and never gives it back
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Bar);

    feed(t, "\033c"); // RIS
    Preferences::Effective::applyCursorPreferences(t, shapeConfiguration(Preferences::CursorShape::Underline));
    CHECK(t.cursorStyle == Cursor::Style::FollowUserPreference);
    CHECK(Cursor::resolve(&t, 0).shape == Cursor::Shape::Underline);
}

/// A reader's pick outranks a style a program left standing; a reload does not.
///
/// The two callers of these preferences want opposite answers about
/// `cursorStyle`. A shell that dresses its prompt sends DECSCUSR at every one
/// of them, so a reader who picks a shape and is answered with the program's
/// has been told the setting does not work — the pick has to clear it. A
/// configuration re-read has no such licence: an unrelated key moving would
/// then revoke what a running program asked for.
TEST(testAPickClearsTheProgramsStyleAndAReloadDoesNot)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    const Preferences::Values configured = shapeConfiguration(Preferences::CursorShape::Bar);
    Preferences::Effective::applyCursorPreferences(t, configured);

    feed(t, "\033[4 q"); // a prompt sets a steady underline
    CHECK(t.cursorStyle == Cursor::Style::SteadyUnderline);

    // The re-seed a config reload runs leaves the program's style standing.
    Preferences::Effective::applyCursorPreferences(t, configured);
    CHECK_MSG(t.cursorStyle == Cursor::Style::SteadyUnderline, "a reload does not revoke DECSCUSR");
    CHECK_MSG(Cursor::resolve(&t, 0).shape == Cursor::Shape::Underline, "and the program's shape is what draws");

    // The pick hands it back, and the configured shape is on screen at once.
    Preferences::Effective::applyPickedCursorPreferences(t, configured);
    CHECK_MSG(t.cursorStyle == Cursor::Style::FollowUserPreference, "a pick hands the style back to the reader");
    CHECK_MSG(Cursor::resolve(&t, 0).shape == Cursor::Shape::Bar, "and the picked shape is what draws");

    // Handing it back is not taking it away.
    feed(t, "\033[4 q");
    CHECK_MSG(t.cursorStyle == Cursor::Style::SteadyUnderline, "the program reclaims the shape the next time it asks");
}

UBIQUITTY_TEST_MAIN()
