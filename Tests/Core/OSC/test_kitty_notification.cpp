// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_kitty_notification.cpp — OSC 99 assembled from its chunks
// (Core/OSC/KittyNotification.h).
//
// A program using kitty's protocol sends a notification's title and its body
// as separate sequences under one name. Read one sequence at a time they were
// two notifications, each promoted to a heading, and the banner that should
// have said who was speaking and what they wanted said one or the other.
//
// What the cases hold to:
//
//   Chunks under one name are one notification, finished by the first chunk
//   that does not hold it open.
//
//   Every quantity a program chooses is bounded: names open at once, bytes in
//   a field and in a name, encoded text decoded per chunk, and the time an
//   unfinished notification is kept.
//
//   A chunk that asks the terminal to answer, or carries something a banner
//   has no slot for, produces nothing and breaks nothing.
//

#include "Harness.h"

#include "Core/OSC/KittyNotification.h"

#include <optional>
#include <string>

namespace {

using Ubiquitty::NotificationOccasion;
using Ubiquitty::Ticks;
using Ubiquitty::OSC::attentionTextCapacity;
using Ubiquitty::OSC::KittyNotification;
using Ubiquitty::OSC::KittyNotificationAssembler;

const std::optional<Ticks> noClock;

/// Whether `got` is a notification with these three fields.
bool is(const std::optional<KittyNotification>& got, std::string_view identifier, std::string_view title, std::string_view body) // bool-ok: a predicate
{
    return got && got->identifier == identifier && got->title == title && got->body == body;
}

} // namespace

TEST(aSingleSequenceIsAWholeNotificationAndItsPayloadIsTheTitle)
{
    KittyNotificationAssembler assembler;
    CHECK(is(assembler.accept(";Hello", noClock), "", "Hello", ""));
    CHECK(is(assembler.accept("Hello with no metadata field", noClock), "", "Hello with no metadata field", ""));
    CHECK(is(assembler.accept("i=7;Named", noClock), "7", "Named", ""));
    CHECK(is(assembler.accept("p=body;Only a body", noClock), "", "", "Only a body"));
    CHECK_EQ(assembler.openCount(), std::size_t { 0 });
}

TEST(aTitleChunkAndABodyChunkUnderOneNameAreOneNotification)
{
    KittyNotificationAssembler assembler;
    CHECK(!assembler.accept("i=1:d=0;Agent", noClock));
    CHECK_EQ(assembler.openCount(), std::size_t { 1 });
    CHECK(is(assembler.accept("i=1:d=1:p=body;Agent is waiting for your input", noClock), "1", "Agent", "Agent is waiting for your input"));
    CHECK_EQ(assembler.openCount(), std::size_t { 0 });

    // The order is the program's: a body may come first.
    CHECK(!assembler.accept("i=2:d=0:p=body;what", noClock));
    CHECK(is(assembler.accept("i=2:p=title;who", noClock), "2", "who", "what"));
}

TEST(aFieldSentInSeveralChunksIsJoined)
{
    KittyNotificationAssembler assembler;
    CHECK(!assembler.accept("i=a:d=0;Build ", noClock));
    CHECK(!assembler.accept("i=a:d=0;finished", noClock));
    CHECK(!assembler.accept("i=a:d=0:p=body;3 warnings", noClock));
    CHECK(!assembler.accept("i=a:d=0:p=body;, 0 errors", noClock));
    // The chunk that finishes it may carry nothing of its own.
    CHECK(is(assembler.accept("i=a;", noClock), "a", "Build finished", "3 warnings, 0 errors"));
}

TEST(notificationsUnderDifferentNamesAssembleSideBySide)
{
    KittyNotificationAssembler assembler;
    CHECK(!assembler.accept("i=one:d=0;First", noClock));
    CHECK(!assembler.accept("i=two:d=0;Second", noClock));
    CHECK(is(assembler.accept("i=two:p=body;b2", noClock), "two", "Second", "b2"));
    CHECK(is(assembler.accept("i=one:p=body;b1", noClock), "one", "First", "b1"));

    // Chunks sent without a name share the nameless one.
    CHECK(!assembler.accept("d=0;T", noClock));
    CHECK(is(assembler.accept("p=body;B", noClock), "", "T", "B"));
}

TEST(aBase64PayloadIsDecodedWithinTheFieldsBound)
{
    KittyNotificationAssembler assembler;
    CHECK(!assembler.accept("i=1:d=0:e=1;QWdlbnQ=", noClock));
    CHECK(is(assembler.accept("i=1:e=1:p=body;d2FpdGluZw==", noClock), "1", "Agent", "waiting"));

    // Text that is not base64 contributes nothing, and the notification still
    // finishes with what it had.
    CHECK(!assembler.accept("i=2:d=0;kept", noClock));
    CHECK(is(assembler.accept("i=2:e=1:p=body;***", noClock), "2", "kept", ""));

    // An encoded payload is read only as far as the field could hold.
    std::string encoded;
    for (int group = 0; group < 4000; ++group)
        encoded += "eHh4"; // "xxx"
    const auto big = assembler.accept("e=1;" + encoded, noClock);
    REQUIRE(big.has_value());
    CHECK_EQ(big->title.size(), attentionTextCapacity);
    CHECK(big->title.find_first_not_of('x') == std::string::npos);
}

TEST(eachFieldIsBoundedHoweverManyChunksFeedIt)
{
    KittyNotificationAssembler assembler;
    const std::string chunk(200, 'y');
    for (int index = 0; index < 50; ++index) {
        CHECK(!assembler.accept("i=1:d=0;" + chunk, noClock));
        CHECK(!assembler.accept("i=1:d=0:p=body;" + chunk, noClock));
    }
    const auto done = assembler.accept("i=1;", noClock);
    REQUIRE(done.has_value());
    CHECK_EQ(done->title.size(), attentionTextCapacity);
    CHECK_EQ(done->body.size(), attentionTextCapacity);
}

TEST(aNameIsReducedToTheProtocolsAlphabetAndBounded)
{
    KittyNotificationAssembler assembler;
    CHECK(is(assembler.accept("i=a/b c\x1b.d_e-f+g;x", noClock), "abc.d_e-f+g", "x", ""));
    const std::string longName(500, 'n');
    const auto named = assembler.accept("i=" + longName + ";x", noClock);
    REQUIRE(named.has_value());
    CHECK_EQ(named->identifier.size(), Ubiquitty::OSC::kittyNotificationIdentifierCapacity);
}

TEST(onlySoManyNotificationsAreHeldOpen)
{
    using Ubiquitty::OSC::maxOpenKittyNotifications;
    KittyNotificationAssembler assembler;
    for (std::size_t index = 0; index < maxOpenKittyNotifications + 3; ++index) {
        CHECK(!assembler.accept("i=n" + std::to_string(index) + ":d=0;title" + std::to_string(index), noClock));
        CHECK(assembler.openCount() <= maxOpenKittyNotifications);
    }
    CHECK_EQ(assembler.openCount(), maxOpenKittyNotifications);
    // The ones begun longest ago were abandoned: finishing one of them finds
    // nothing held and has only its own words.
    CHECK(is(assembler.accept("i=n0:p=body;late", noClock), "n0", "", "late"));
    // The newest are intact.
    CHECK(is(assembler.accept("i=n6:p=body;b", noClock), "n6", "title6", "b"));
}

TEST(anUnfinishedNotificationIsAbandonedAfterItsTime)
{
    using Ubiquitty::OSC::kittyNotificationAssemblyTicks;
    KittyNotificationAssembler assembler;
    const Ticks start(1000);
    CHECK(!assembler.accept("i=1:d=0;stale title", start));
    CHECK(!assembler.accept("i=2:d=0;other", start + 10));
    // Within its time, a chunk joins.
    CHECK(!assembler.accept("i=1:d=0:p=body;still ", start + kittyNotificationAssemblyTicks));
    CHECK_EQ(assembler.openCount(), std::size_t { 2 });
    // Past it, what was open is gone before the chunk is read.
    CHECK(is(assembler.accept("i=1:p=body;fresh", start + kittyNotificationAssemblyTicks + 1), "1", "", "fresh"));
    // Each is timed from its own first chunk: the one begun ten ticks later
    // has ten left, and then goes too.
    CHECK_EQ(assembler.openCount(), std::size_t { 1 });
    CHECK(is(assembler.accept("i=2:p=body;late", start + kittyNotificationAssemblyTicks + 11), "2", "", "late"));
    CHECK_EQ(assembler.openCount(), std::size_t { 0 });

    // The clock wraps; elapsed time is still measured across the wrap.
    const Ticks nearWrap(0xFFFFFFF0u);
    CHECK(!assembler.accept("i=w:d=0;T", nearWrap));
    CHECK(is(assembler.accept("i=w:p=body;B", nearWrap + 0x20), "w", "T", "B"));

    // A terminal with no clock expires nothing.
    CHECK(!assembler.accept("i=c:d=0;T", noClock));
    CHECK(is(assembler.accept("i=c:p=body;B", start + 1000000), "c", "T", "B"));
}

TEST(theOccasionAProgramNamesTravelsWithTheNotification)
{
    KittyNotificationAssembler assembler;
    CHECK(assembler.accept(";x", noClock)->occasion == NotificationOccasion::Always);
    CHECK(assembler.accept("o=always;x", noClock)->occasion == NotificationOccasion::Always);
    CHECK(assembler.accept("o=unfocused;x", noClock)->occasion == NotificationOccasion::Unfocused);
    CHECK(assembler.accept("o=invisible;x", noClock)->occasion == NotificationOccasion::Invisible);
    CHECK(assembler.accept("o=whenever;x", noClock)->occasion == NotificationOccasion::Always);
    // Stated on the first chunk, it holds for the notification.
    CHECK(!assembler.accept("i=1:d=0:o=unfocused;T", noClock));
    CHECK(assembler.accept("i=1:p=body;B", noClock)->occasion == NotificationOccasion::Unfocused);
}

TEST(unknownKeysAndOnesSetAsideAreIgnored)
{
    KittyNotificationAssembler assembler;
    CHECK(is(assembler.accept("i=1:u=2:w=5000:s=c2lsZW50:f=QXBw:t=aW0=:a=focus:zz=1:=:novalue;Title", noClock), "1", "Title", ""));
    // `t=` is a type tag in this protocol, never a title.
    CHECK(is(assembler.accept("t=Alert;Body text", noClock), "", "Body text", ""));
}

TEST(aChunkThatAsksForAnAnswerOrCarriesNoWordsProducesNothing)
{
    KittyNotificationAssembler assembler;
    CHECK(!assembler.accept("i=1:p=?;", noClock));
    CHECK(!assembler.accept("i=1:p=alive;", noClock));
    CHECK(!assembler.accept("i=1:p=icon:e=1;aWNvbg==", noClock));
    CHECK(!assembler.accept("i=1:p=buttons;Yes\xe2\x80\xa8No", noClock));
    CHECK(!assembler.accept("i=1:p=unheard-of;text", noClock));
    CHECK_EQ(assembler.openCount(), std::size_t { 0 });

    // A notification with no words is not one.
    CHECK(!assembler.accept("i=1;", noClock));
    CHECK(!assembler.accept("", noClock));
    CHECK(!assembler.accept(";", noClock));

    // An icon chunk in the middle of a notification leaves its words alone.
    CHECK(!assembler.accept("i=2:d=0;Title", noClock));
    CHECK(!assembler.accept("i=2:d=0:p=icon:e=1;aWNvbg==", noClock));
    CHECK(is(assembler.accept("i=2:p=body;Body", noClock), "2", "Title", "Body"));

    // Closing a name abandons what was open under it and shows nothing.
    CHECK(!assembler.accept("i=3:d=0;Half", noClock));
    CHECK(!assembler.accept("i=3:p=close;", noClock));
    CHECK_EQ(assembler.openCount(), std::size_t { 0 });
}

UBIQUITTY_TEST_MAIN()
