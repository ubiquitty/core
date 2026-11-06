// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// test_composeengine.cpp — the portable compose-key state machine. Runs
// identically on every frontend; a frontend only decides which key is
// "compose" and feeds printable codepoints.
//

#include "Core/Input/ComposeEngine.h"

#include <cstdio>
#include <vector>

using namespace Ubiquitty::Input;

TEST(testInactiveWhenNotBegun)
{
    ComposeEngine e;
    CHECK(!e.active());
    CHECK(e.feed('a') == ComposeResult::Inactive);
}

TEST(testTwoKeyAcute)
{
    ComposeEngine e;
    e.begin();
    CHECK(e.active());
    CHECK(e.feed('\'') == ComposeResult::Pending); // ' is a prefix
    CHECK(e.active());
    CHECK(e.feed('e') == ComposeResult::Composed); // ' e -> é
    CHECK(e.result() == 0x00E9);
    CHECK(!e.active()); // resets after composing
}

TEST(testTwoKeyUppercaseAndTilde)
{
    ComposeEngine e;
    e.begin();
    CHECK(e.feed('~') == ComposeResult::Pending);
    CHECK(e.feed('N') == ComposeResult::Composed); // ~ N -> Ñ
    CHECK(e.result() == 0x00D1);
}

TEST(testThreeKeyEmDash)
{
    ComposeEngine e;
    e.begin();
    CHECK(e.feed('-') == ComposeResult::Pending);
    CHECK(e.feed('-') == ComposeResult::Pending);
    CHECK(e.feed('-') == ComposeResult::Composed); // - - - -> em dash
    CHECK(e.result() == 0x2014);
}

TEST(testInvalidSequenceResets)
{
    ComposeEngine e;
    e.begin();
    CHECK(e.feed('\'') == ComposeResult::Pending);
    CHECK(e.feed('z') == ComposeResult::Invalid); // ' z is not a thing
    CHECK(!e.active());
    // After Invalid the engine is inactive; the next key passes through.
    CHECK(e.feed('x') == ComposeResult::Inactive);
}

TEST(testFirstKeyUnknownIsInvalid)
{
    ComposeEngine e;
    e.begin();
    // 'q' starts no compose entry.
    CHECK(e.feed('q') == ComposeResult::Invalid);
    CHECK(!e.active());
}

TEST(testCurrencyAndSymbols)
{
    ComposeEngine e;
    e.begin();
    CHECK(e.feed('c') == ComposeResult::Pending);
    CHECK(e.feed('=') == ComposeResult::Composed); // c = -> €
    CHECK(e.result() == 0x20AC);

    e.begin();
    CHECK(e.feed('<') == ComposeResult::Pending);
    CHECK(e.feed('3') == ComposeResult::Composed); // < 3 -> ♥
    CHECK(e.result() == 0x2665);

    e.begin();
    CHECK(e.feed('1') == ComposeResult::Pending);
    CHECK(e.feed('/') == ComposeResult::Pending);
    CHECK(e.feed('2') == ComposeResult::Composed); // 1 / 2 -> ½
    CHECK(e.result() == 0x00BD);
}

TEST(testResetAborts)
{
    ComposeEngine e;
    e.begin();
    CHECK(e.feed('\'') == ComposeResult::Pending);
    e.reset();
    CHECK(!e.active());
    CHECK(e.feed('e') == ComposeResult::Inactive); // no longer composing
}

TEST(testBeginRestartsMidsequence)
{
    ComposeEngine e;
    e.begin();
    CHECK(e.feed('\'') == ComposeResult::Pending);
    e.begin(); // user pressed compose again — restart
    CHECK(e.feed('`') == ComposeResult::Pending);
    CHECK(e.feed('a') == ComposeResult::Composed); // ` a -> à
    CHECK(e.result() == 0x00E0);
}

// The ladder over the engine: which key arms it, which keys it spends, and
// what a frontend is told to write. Every frontend asks this one object, so
// no two of them can disagree about a non-printable key.

namespace {

// A `[keys]`-armed state with the compose key on Right Alt's usual spelling.
ComposeState armed(const char* spec = "ctrl+space")
{
    ComposeState state;
    state.rebuild(spec);
    return state;
}

// The chord `spec` names, for feeding as the trigger.
KeyChord chordOf(const char* spec)
{
    std::vector<KeyChord> sequence;
    parseKeySpec(spec, sequence);
    return sequence.empty() ? KeyChord {} : sequence.front();
}

} // namespace

TEST(testUnarmedPassesEveryKey)
{
    ComposeState state;
    CHECK(!state.isArmed());
    // No compose key configured is the common case, and it must cost the key
    // nothing: an unarmed state that swallowed anything would eat typing.
    CHECK(state.offer(chordOf("ctrl+space"), '\'') == ComposeAction::Pass);
    CHECK(state.offer(KeyChord { 'a', 0 }, 'a') == ComposeAction::Pass);
}

TEST(testTriggerThenSequenceComposes)
{
    ComposeState state = armed();
    CHECK(state.isArmed());
    CHECK(state.offer(chordOf("ctrl+space"), 0) == ComposeAction::Swallow);
    CHECK(state.offer(KeyChord { '\'', 0 }, '\'') == ComposeAction::Swallow);
    CHECK(state.offer(KeyChord { 'e', 0 }, 'e') == ComposeAction::Write);
    CHECK(state.composed() == 0x00E9); // ' e -> é
}

TEST(testANonPrintableKeyAbortsTheSequence)
{
    ComposeState state = armed();
    state.offer(chordOf("ctrl+space"), 0);
    CHECK(state.offer(KeyChord { '\'', 0 }, '\'') == ComposeAction::Swallow);
    // An arrow key produces no character. Two things follow: it takes its own
    // path rather than being eaten, and it ends the capture — staying armed
    // while the reader moves the cursor turns the next quote they type into
    // something they did not ask for, a long way from the key that armed it.
    CHECK(state.offer(KeyChord { 0xF702, 0 }, 0) == ComposeAction::Pass);
    CHECK(state.offer(KeyChord { 'e', 0 }, 'e') == ComposeAction::Pass);
}

TEST(testAnUnknownSequenceIsSpentNotWritten)
{
    ComposeState state = armed();
    state.offer(chordOf("ctrl+space"), 0);
    CHECK(state.offer(KeyChord { '\'', 0 }, '\'') == ComposeAction::Swallow);
    // No entry starts with ' q: the key is spent rather than reaching the
    // host, and the capture is over.
    CHECK(state.offer(KeyChord { 'q', 0 }, 'q') == ComposeAction::Swallow);
    CHECK(state.offer(KeyChord { 'a', 0 }, 'a') == ComposeAction::Pass);
}

TEST(testComposeIsOneKey)
{
    ComposeState state;
    // A chord sequence names a leader, not a compose key. Rejected whole
    // rather than half-honoured by taking its first chord: a compose key you
    // have to type twice is not the model this implements.
    state.rebuild("ctrl+a ctrl+x");
    CHECK(!state.isArmed());
    // And an unparseable spec arms nothing rather than arming a zero chord,
    // which every key with no modifiers would match.
    state.rebuild("not-a-key");
    CHECK(!state.isArmed());
    state.rebuild("");
    CHECK(!state.isArmed());
}

TEST(testRebuildDropsASequenceInProgress)
{
    ComposeState state = armed();
    state.offer(chordOf("ctrl+space"), 0);
    CHECK(state.offer(KeyChord { '\'', 0 }, '\'') == ComposeAction::Swallow);
    // The reader just changed what starts a sequence, so the half-typed one is
    // not finished against the new trigger.
    state.rebuild("ctrl+k");
    CHECK(state.offer(KeyChord { 'e', 0 }, 'e') == ComposeAction::Pass);
    CHECK(state.offer(chordOf("ctrl+k"), 0) == ComposeAction::Swallow);
}

TEST(testComposeTerminalPunctuation)
{
    struct Case {
        const char* sequence;
        std::uint32_t expected;
    };
    const Case cases[] = {
        { "at", '@' },
        { "((", '[' },
        { "))", ']' },
        { "(-", '{' },
        { "-)", '}' },
        { "//", '\\' },
        { "!^", '|' },
        { "~ ", '~' },
    };
    for (const auto& item : cases) {
        ComposeState state;
        state.rebuild("ctrl+space");
        CHECK(state.isArmed());
        CHECK(state.offer(chordOf("ctrl+space"), 0) == ComposeAction::Swallow);
        CHECK(state.offer(KeyChord { static_cast<std::uint32_t>(item.sequence[0]), 0 }, item.sequence[0]) == ComposeAction::Swallow);
        CHECK(state.offer(KeyChord { static_cast<std::uint32_t>(item.sequence[1]), 0 }, item.sequence[1]) == ComposeAction::Write);
        CHECK(state.composed() == item.expected);
    }
    ComposeState menu;
    menu.rebuild("menu");
    CHECK(menu.isArmed());
}

UBIQUITTY_TEST_MAIN()
