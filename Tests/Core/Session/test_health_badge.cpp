// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_health_badge.cpp — the remote status badge's two shared facts
// (Core/Session/HealthBadge.h).
//
// A window wearing a remote badge reads one state and draws one indicator. The
// fold from a connection's phases to that state, and the two decisions a badge
// makes off it, are here rather than in each frontend for the reason every
// duplicated policy is: three frontends folding the phases themselves is three
// vocabularies, and the one that drifts is the one nobody is looking at.
//
// What the cases hold to:
//
//   Idle folds into Connecting. A session whose attach has not started shows
//   the same badge as one whose attach is in flight; a frontend that mapped
//   Idle to Connected would paint a steady link over a session with none.
//
//   Every other phase keeps its own name, and the two enumerations disagree
//   about the numbers — `Connected` is 0 in one and 2 in the other — so a fold
//   written as a cast passes for Connecting and lies about the steady state.
//
//   Only the steady state is steady, and only it names a carrier. Both are the
//   badge's whole restraint: an indicator tinted at rest is one people read
//   past, and a carrier named before bytes ride one describes the attempt.
//

#include "Harness.h"

#include "Core/Session/HealthBadge.h"

#include <string>

namespace {

namespace Session = Ubiquitty::Session;

} // namespace

TEST(everyConnectionPhaseFoldsToTheStateABadgeDraws)
{
    CHECK(Session::healthFor(Session::State::Connected) == Session::Health::Connected);
    CHECK(Session::healthFor(Session::State::Deploying) == Session::Health::Deploying);
    CHECK(Session::healthFor(Session::State::Reconnecting) == Session::Health::Reconnecting);
    CHECK(Session::healthFor(Session::State::Failed) == Session::Health::Failed);
    CHECK(Session::healthFor(Session::State::Connecting) == Session::Health::Connecting);
    // The fold that is a decision rather than a rename: there is no pre-start
    // badge, so a session that has not been started yet reads as one that has
    // not connected yet.
    CHECK(Session::healthFor(Session::State::Idle) == Session::Health::Connecting);
}

TEST(theFoldIsNotTheCastTheNumbersWouldAllow)
{
    // Both enumerations are std::uint8_t and a cast between them compiles at
    // every value, so this is the case that catches one: the phases and the
    // states agree on Connecting and Failed and disagree on the steady state,
    // which is where a session spends its life.
    CHECK(static_cast<std::uint8_t>(Session::State::Connected) == 2);
    CHECK(static_cast<std::uint8_t>(Session::Health::Connected) == 0);
    CHECK(static_cast<std::uint8_t>(Session::healthFor(Session::State::Connected)) == 0);
    CHECK(static_cast<std::uint8_t>(Session::State::Deploying) == 5);
    CHECK(static_cast<std::uint8_t>(Session::healthFor(Session::State::Deploying)) == 2);
}

TEST(aBadgeIsQuietAndNamesItsCarrierOnlyWhileTheLinkIsSteady)
{
    const Session::HealthBadge connected = Session::badgeFor(Session::Health::Connected);
    CHECK(connected.isSteady);
    CHECK(connected.shouldNameCarrier);

    for (const auto health : { Session::Health::Connecting, Session::Health::Deploying,
             Session::Health::Reconnecting, Session::Health::Failed }) {
        const Session::HealthBadge badge = Session::badgeFor(health);
        CHECK(!badge.isSteady);
        // A link being stood up, retried or given up on has no carrier to
        // name: naming one there would report a transport the bytes are not
        // riding.
        CHECK(!badge.shouldNameCarrier);
    }
}

TEST(onlyAHostSessionHasAPlaceAndThisMachineIsOneOfThem)
{
    CHECK(Session::hostPlaceFor({}) == Session::HostPlace::None);
    // A plain local shell is not on this machine's daemon, whatever the flag
    // beside it says: without a host session there is no host to name.
    CHECK(Session::hostPlaceFor({ .isHostSession = false, .isOnThisMachine = true }) == Session::HostPlace::None);
    CHECK(Session::hostPlaceFor({ .isHostSession = true, .isOnThisMachine = true }) == Session::HostPlace::ThisMachine);
    CHECK(Session::hostPlaceFor({ .isHostSession = true, .isOnThisMachine = false }) == Session::HostPlace::Remote);
}

TEST(theHostChipStandsInEveryLinkStateIncludingTheSteadyOne)
{
    // The defect this pins: a chip that comes down once the link settles
    // leaves a connected window saying nothing about which machine it is on.
    for (const auto place : { Session::HostPlace::ThisMachine, Session::HostPlace::Remote }) {
        for (const auto health : { Session::Health::Connected, Session::Health::Connecting, Session::Health::Deploying,
                 Session::Health::Reconnecting, Session::Health::Failed }) {
            const Session::HostChip chip = Session::hostChipFor(place, health);
            CHECK(chip.isShown);
            CHECK(chip.isSteady == (health == Session::Health::Connected));
        }
    }
    // A plain local shell wears none, in any state.
    for (const auto health : { Session::Health::Connected, Session::Health::Failed }) {
        const Session::HostChip chip = Session::hostChipFor(Session::HostPlace::None, health);
        CHECK(!chip.isShown);
        CHECK(!chip.shouldNameCarrier);
    }
}

TEST(onlyARemoteHostNamesItsCarrier)
{
    CHECK(Session::hostChipFor(Session::HostPlace::Remote, Session::Health::Connected).shouldNameCarrier);
    // This machine's daemon is reached over its own socket: naming ssh or quic
    // beside it would describe a network hop that is not there.
    CHECK(!Session::hostChipFor(Session::HostPlace::ThisMachine, Session::Health::Connected).shouldNameCarrier);
    CHECK(!Session::hostChipFor(Session::HostPlace::Remote, Session::Health::Reconnecting).shouldNameCarrier);
}

TEST(paneBarsNameTheHostOnlyWhereAWindowMixesPlaces)
{
    const std::string laptop = "laptop";
    const std::string build = "build-box";
    const std::string none;

    {
        Session::PaneHostMix mix;
        CHECK(!mix.shouldNameHost());
        mix.add(Session::HostPlace::Remote, laptop);
        mix.add(Session::HostPlace::Remote, laptop);
        CHECK_MSG(!mix.shouldNameHost(), "panes of one host say so once, in the title row");
    }
    {
        Session::PaneHostMix mix;
        mix.add(Session::HostPlace::None, none);
        mix.add(Session::HostPlace::None, none);
        CHECK(!mix.shouldNameHost());
    }
    {
        Session::PaneHostMix mix;
        mix.add(Session::HostPlace::None, none);
        mix.add(Session::HostPlace::ThisMachine, none);
        CHECK_MSG(mix.shouldNameHost(), "a pane shared into this machine's daemon beside a plain shell");
    }
    {
        Session::PaneHostMix mix;
        mix.add(Session::HostPlace::Remote, laptop);
        mix.add(Session::HostPlace::Remote, build);
        CHECK_MSG(mix.shouldNameHost(), "two hosts in one window");
    }
    {
        // The key is read only for a remote host: a plain shell and this
        // machine's daemon are each one place whatever they are called.
        Session::PaneHostMix mix;
        mix.add(Session::HostPlace::ThisMachine, laptop);
        mix.add(Session::HostPlace::ThisMachine, build);
        CHECK(!mix.shouldNameHost());
    }
}

UBIQUITTY_TEST_MAIN()
