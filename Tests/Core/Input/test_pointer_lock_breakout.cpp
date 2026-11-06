// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_pointer_lock_breakout.cpp — pins the pointer-lock break-out gesture
// (Source/Core/Input/PointerLockBreakout.h): the hold-Control+Command arming logic
// and the engage-hint policy.
//
// The gesture is the MANDATORY escape from relative pointer lock, so its contract
// is safety-critical: it arms only on Control+Command held alone (any other key or
// modifier is a normal shortcut, not the break-out), and fires only once the pair
// has been held past the threshold, so a chord passed through in transit cannot
// trip it. These checks pin the arming predicate, the hold threshold, and
// the "don't nag" hint policy.

#include "Core/Input/PointerLockBreakout.h"
#include "Core/Input/PointerLockLifecycle.h"
#include "Core/Input/TransientHint.h"

#include <cstdio>
#include <cstring>

using Ubiquitty::Input::isPointerLockBreakoutArmed;
using Ubiquitty::Input::pointerLockBreakoutHoldTicks;
using Ubiquitty::Input::PointerLockFacts;
using Ubiquitty::Input::PointerLockKeyState;
using Ubiquitty::Input::PointerLockMode;
using Ubiquitty::Input::PointerLockVerdict;
using Ubiquitty::Input::resolvePointerLock;

// The armed state (⌃⌘ held alone) and a plainly-disarmed state, reused below.
static constexpr PointerLockKeyState armedKeys { .isControlHeld = true, .isCommandHeld = true };
static constexpr PointerLockKeyState noKeys {};

// Armed only when Control+Command are the sole modifiers and no other key is
// down. Anything else is an ordinary chord, not the break-out.
TEST(testArmingPredicate)
{
    CHECK(isPointerLockBreakoutArmed(armedKeys)); // ⌃⌘ alone → armed
    CHECK(!isPointerLockBreakoutArmed(PointerLockKeyState { .isControlHeld = true })); // missing Command
    CHECK(!isPointerLockBreakoutArmed(PointerLockKeyState { .isCommandHeld = true })); // missing Control
    CHECK(!isPointerLockBreakoutArmed(
        PointerLockKeyState { .isControlHeld = true, .isCommandHeld = true, .isShiftHeld = true })); // + Shift
    CHECK(!isPointerLockBreakoutArmed(
        PointerLockKeyState { .isControlHeld = true, .isCommandHeld = true, .isOptionHeld = true })); // + Option
    CHECK(!isPointerLockBreakoutArmed(PointerLockKeyState {
        .isControlHeld = true, .isCommandHeld = true, .isAnyNonModifierKeyHeld = true })); // + a key (⌃⌘C)
    CHECK(!isPointerLockBreakoutArmed(noKeys)); // nothing held
}

// The hold is a fixed reserved constant in the ticks every arm counts: 500 ms
// at 60 Hz. Both ends of the range defeat the safety valve, so it is not
// config-tunable. The hold itself is exercised through the ladder
// (test_pointer_ladder), where the clock lives.
TEST(testHoldThreshold)
{
    CHECK(pointerLockBreakoutHoldTicks == 30);
}

// The grab is held only while both hold: the application asks and the surface is the
// live input target: a backgrounded surface must not hold a pointer no window
// owns, and takes it back on return without the application re-sending 7403.
TEST(testLockVerdict)
{
    CHECK(resolvePointerLock(PointerLockMode::Set, PointerLockFacts { .isSurfaceFocused = true }) == PointerLockVerdict::Hold);
    CHECK(resolvePointerLock(PointerLockMode::Set, PointerLockFacts { .isSurfaceFocused = false }) == PointerLockVerdict::Release);
    CHECK(resolvePointerLock(PointerLockMode::Clear, PointerLockFacts { .isSurfaceFocused = true }) == PointerLockVerdict::Release);
    CHECK(resolvePointerLock(PointerLockMode::Clear, PointerLockFacts { .isSurfaceFocused = false }) == PointerLockVerdict::Release);
}

// A real ⌃⌘ shortcut (a key joins) can never break out, even if the modifiers
// were held long — the arming predicate is false the moment the key lands.
TEST(testShortcutNeverBreaksOut)
{
    constexpr PointerLockKeyState shortcut {
        .isControlHeld = true, .isCommandHeld = true, .isAnyNonModifierKeyHeld = true
    };
    CHECK(!isPointerLockBreakoutArmed(shortcut));
}

// The way out is taught the first few locks, then stops nagging. The words
// and the budget are the shared hint vocabulary's (TransientHint.h), so the
// lock names the same keys the chord above reads.
TEST(testHintPolicy)
{
    CHECK(Ubiquitty::Input::hintMaxShows == 3);
    CHECK(Ubiquitty::Input::shouldShowHint(0));
    CHECK(Ubiquitty::Input::shouldShowHint(2));
    CHECK(!Ubiquitty::Input::shouldShowHint(3));
    CHECK(!Ubiquitty::Input::shouldShowHint(-1)); // defensive: a bad count never nags
    const char* text = Ubiquitty::Input::hintText(Ubiquitty::Input::Hint::PointerLockRelease, Ubiquitty::Input::ModifierVocabulary::Mac);
    // The hint names the keys it uses the way the Human Interface Guidelines
    // spell them: the words, not the keycap glyphs.
    CHECK(text != nullptr && std::strlen(text) > 0);
    CHECK(std::strstr(text, "Control") != nullptr);
    CHECK(std::strstr(text, "Command") != nullptr);
    CHECK(std::strstr(text, "\xE2\x8C") == nullptr); // no U+23xx keycap glyph anywhere
    CHECK(Ubiquitty::Input::isHintPersistent(Ubiquitty::Input::Hint::PointerLockRelease));
}

// The decision is a constant expression — the frontend can branch on it without
// runtime cost.
TEST(testConstexprUsable)
{
    static_assert(isPointerLockBreakoutArmed(armedKeys), "⌃⌘ alone arms at compile time");
    static_assert(resolvePointerLock(PointerLockMode::Set, PointerLockFacts { .isSurfaceFocused = true }) == PointerLockVerdict::Hold,
        "mode set on a focused surface holds at compile time");
    static_assert(resolvePointerLock(PointerLockMode::Set, PointerLockFacts {}) == PointerLockVerdict::Release,
        "an unfocused surface never holds");
    static_assert(Ubiquitty::Input::shouldShowHint(0), "first lock shows the hint");
    CHECK(true);
}

// What an application transition asks of every grab the process holds.
//
// The grab is process-global, and the surface that could answer for it is
// exactly the one an application-level event cannot reach: a pane torn down
// without its teardown running, or one sitting behind the frontmost window
// with no edge of its own. So leaving and going away are releases, which need
// no answer from anybody, and only coming back is a reconcile.
TEST(testAnApplicationTransitionSweepsEveryGrab)
{
    using Ubiquitty::Input::AppLifecycle;
    using Ubiquitty::Input::pointerLockSweep;
    using Ubiquitty::Input::PointerLockSweep;

    CHECK_MSG(pointerLockSweep(AppLifecycle::Deactivated) == PointerLockSweep::ReleaseEveryGrab,
        "a backgrounded application holds no pointer");
    CHECK_MSG(pointerLockSweep(AppLifecycle::Terminating) == PointerLockSweep::ReleaseEveryGrab,
        "and one going away leaves none stranded");
    CHECK_MSG(pointerLockSweep(AppLifecycle::Activated) == PointerLockSweep::ReconcileEveryGrab,
        "coming back asks each surface rather than re-engaging them all");
}

UBIQUITTY_TEST_MAIN()
