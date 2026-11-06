// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The words a list of a host's sessions is drawn with: every phrase, every
// sentence built from a name or a count, the state of a host's section with
// each wording of it, and the default button of a connect.

// Frontends may expose std::quoted before the interface. Its std::string
// overload must not replace the workspace-label formatter through ADL.
#include <iomanip>

#include "Core/Runtime/SessionListWords.h"
#include "Harness.h"

#include <set>
#include <string>

using namespace Ubiquitty;
using Protocol::Sync::ByeReason;
using Protocol::Sync::OlderSide;
using Runtime::ConnectAction;
using Runtime::ConnectActionEffect;
using Runtime::ConnectActionFacts;
using Runtime::HostListingProgress;
using Runtime::HostSectionFacts;
using Runtime::HostSectionState;
using Runtime::HostSectionWords;
using Runtime::SessionListPhrase;
using Session::AttachOpening;
using Session::AttachPlan;
using Session::ListingFailure;

TEST(everyPhraseHasWordsAndAKeyOfItsOwn)
{
    std::set<std::string> keys;
    for (std::size_t index = 0; index < Runtime::sessionListPhraseCount; ++index) {
        const auto phrase = static_cast<SessionListPhrase>(index);
        CHECK_MSG(!Runtime::sessionListPhraseWords(phrase).empty(), std::to_string(index).c_str());
        CHECK_MSG(!Runtime::sessionListPhraseKey(phrase).empty(), std::to_string(index).c_str());
        CHECK_MSG(keys.insert(std::string(Runtime::sessionListPhraseKey(phrase))).second, "two phrases share a key");
        CHECK(Runtime::sessionListPhrase(static_cast<std::uint8_t>(index)) == Runtime::sessionListPhraseWords(phrase));
    }
    // Past the last, nothing.
    CHECK(Runtime::sessionListPhrase(static_cast<std::uint8_t>(Runtime::sessionListPhraseCount)).empty());
    CHECK(Runtime::sessionListPhraseWords(static_cast<SessionListPhrase>(250)).empty());
    CHECK(Runtime::sessionListPhraseKey(static_cast<SessionListPhrase>(250)).empty());
    // The numbers a mirror pins, and the keys the page reads them by.
    CHECK_EQ(std::to_underlying(SessionListPhrase::Connect), 0);
    CHECK_EQ(std::to_underlying(SessionListPhrase::HoldsHere), 43);
    CHECK_STR_EQ(std::string(Runtime::sessionListPhraseKey(SessionListPhrase::Connect)), "connect");
    CHECK_STR_EQ(std::string(Runtime::sessionListPhraseKey(SessionListPhrase::MoveTabToWindow)), "moveTabToWindow");
    CHECK_STR_EQ(std::string(Runtime::sessionListPhraseKey(SessionListPhrase::LastUsed)), "lastUsed");
    CHECK_STR_EQ(std::string(Runtime::sessionListPhraseKey(SessionListPhrase::HoldsHere)), "holdsHere");
    CHECK_STR_EQ(std::string(Runtime::sessionListPhraseWords(SessionListPhrase::RenameSession)), "Rename Workspace…");
    CHECK_STR_EQ(std::string(Runtime::sessionListPhraseWords(SessionListPhrase::Ended)), "ended");
}

TEST(aSentenceIsBuiltFromANameACountOrAnAge)
{
    using Session::Identity::ObjectKind;
    CHECK_STR_EQ(Runtime::sessionCountWords(0), "no workspaces");
    CHECK_STR_EQ(Runtime::sessionCountWords(1), "1 workspace");
    CHECK_STR_EQ(Runtime::sessionCountWords(3), "3 workspaces");
    // A workspace is a session to a person; the session behind a pane is
    // the pane's.
    CHECK_STR_EQ(std::string(Runtime::levelMenuTitle(ObjectKind::Workspace)), "Workspace…");
    CHECK_STR_EQ(std::string(Runtime::levelMenuTitle(ObjectKind::Session)), "Pane’s Session…");
    CHECK_STR_EQ(std::string(Runtime::levelMenuTitle(ObjectKind::Window)), "Window…");
    CHECK_STR_EQ(std::string(Runtime::levelMenuTitle(ObjectKind::Tab)), "Tab…");
    CHECK_STR_EQ(std::string(Runtime::levelMenuTitle(ObjectKind::Pane)), "Pane…");
    CHECK_STR_EQ(std::string(Runtime::levelMenuTitle(ObjectKind::Client)), "Client…");
    for (std::uint8_t kind = 0; kind < Session::Identity::objectKindCount; ++kind)
        CHECK(Runtime::levelMenuTitleWords(kind) == Runtime::levelMenuTitle(static_cast<ObjectKind>(kind)));
    CHECK(Runtime::levelMenuTitleWords(Session::Identity::objectKindCount).empty());
    CHECK_STR_EQ(Runtime::collapseControlWords("build"), "Collapse build");
    CHECK_STR_EQ(Runtime::expandControlWords("build"), "Expand build");
    CHECK_STR_EQ(Runtime::labelChipWords("nightly"), "Label nightly");
    CHECK_STR_EQ(Runtime::tagChipWords("production"), "Tag production");
    CHECK_STR_EQ(Runtime::gridWords(132, 43), "132×43");
    CHECK(Runtime::gridWords(0, 43).empty() && Runtime::gridWords(80, 0).empty() && Runtime::gridWords(-1, -1).empty());
    // An age is its largest whole unit.
    CHECK_STR_EQ(Runtime::lastOutputWords(0), "Last output just now");
    CHECK_STR_EQ(Runtime::lastOutputWords(4), "Last output just now");
    CHECK_STR_EQ(Runtime::lastOutputWords(5), "Last output 5s ago");
    CHECK_STR_EQ(Runtime::lastOutputWords(59), "Last output 59s ago");
    CHECK_STR_EQ(Runtime::lastInputWords(60), "Last input 1m ago");
    CHECK_STR_EQ(Runtime::lastInputWords(3599), "Last input 59m ago");
    CHECK_STR_EQ(Runtime::lastActiveWords(3600), "Last active 1h ago");
    CHECK_STR_EQ(Runtime::lastActiveWords(86'399), "Last active 23h ago");
    CHECK_STR_EQ(Runtime::lastActiveWords(86'400), "Last active 1d ago");
    CHECK_STR_EQ(Runtime::compactAgeWords(0), "just now");
    CHECK_STR_EQ(Runtime::compactAgeWords(4), "just now");
    CHECK_STR_EQ(Runtime::compactAgeWords(5), "5s ago");
    CHECK_STR_EQ(Runtime::compactAgeWords(59), "59s ago");
    CHECK_STR_EQ(Runtime::compactAgeWords(60), "1m ago");
    CHECK_STR_EQ(Runtime::compactAgeWords(3599), "59m ago");
    CHECK_STR_EQ(Runtime::compactAgeWords(3600), "1h ago");
    CHECK_STR_EQ(Runtime::compactAgeWords(86'399), "23h ago");
    CHECK_STR_EQ(Runtime::compactAgeWords(86'400), "1d ago");
    CHECK_STR_EQ(Runtime::sessionNameTooLongSentence(), "A workspace name holds at most 4096 bytes.");
    CHECK_STR_EQ(Runtime::sessionNameTakenSentence("ci"), "“ci” is already on the host; this opens it rather than starting a second one.");
    CHECK_STR_EQ(Runtime::openExistingTitle("ci"), "Open Existing “ci”");
    CHECK_STR_EQ(Runtime::unnamedSessionWords(12), "session 12");
}

TEST(sessionDetailsKeepProcessPlacementOriginSharingAndLifetime)
{
    using Session::Identity::ObjectKind;
    Session::SessionTree tree;
    Session::TreeWorkspace workspace;
    workspace.id = 1;
    workspace.name = "work";
    workspace.endsWhenDetached = true;
    Session::TreeWindow window;
    window.id = Session::Layout::SurfaceID { 2 };
    Session::TreeTab tab;
    tab.id = Session::Layout::TabID { 3 };
    Session::TreePane pane;
    pane.id = Session::Layout::PaneID { 4 };
    pane.sessionID = 5;
    pane.program = "vim";
    pane.createdUnixSeconds = 1234;
    pane.isAdopted = true;
    pane.attendance.people = 2;
    tab.panes = { pane };
    window.tabs = { tab };
    workspace.windows = { window };
    tree.workspaces = { workspace };
    for (const Session::Identity::ObjectID target : { Session::Identity::ObjectID { ObjectKind::Workspace, 1 }, { ObjectKind::Window, 2 }, { ObjectKind::Tab, 3 }, { ObjectKind::Pane, 4 }, { ObjectKind::Session, 5 } }) {
        const auto details = Runtime::hostSessionDetails(tree, target);
        REQUIRE(details.size() == 1);
        const auto& detail = details.front();
        CHECK_EQ(detail.sessionID, 5u);
        CHECK_EQ(detail.createdUnixSeconds, 1234u);
        CHECK_STR_EQ(detail.title, "vim");
        CHECK_STR_EQ(detail.placement, "work · Window 2 · Tab 3 · Pane 4");
        CHECK_STR_EQ(detail.creationLabel, "Hosted since");
        CHECK_STR_EQ(detail.origin, "Adopted");
        CHECK_STR_EQ(detail.sharing, "Multiple clients");
        CHECK_STR_EQ(detail.lifetime, "Ends after the last client leaves");
    }
    auto& held = tree.workspaces.front();
    held.endsWhenDetached = false;
    auto& single = held.windows.front().tabs.front().panes.front();
    single.program.clear();
    single.isAdopted = false;
    single.createdUnixSeconds = 0;
    single.attendance.people = 1;
    auto details = Runtime::hostSessionDetails(tree, { ObjectKind::Pane, 4 });
    REQUIRE(details.size() == 1);
    CHECK_STR_EQ(details.front().title, "Session 5");
    CHECK_EQ(details.front().createdUnixSeconds, 0u);
    CHECK_STR_EQ(details.front().creationLabel, "Started");
    CHECK_STR_EQ(details.front().unknownCreationTime, "Not recorded");
    CHECK_STR_EQ(details.front().origin, "Host process");
    CHECK_STR_EQ(details.front().sharing, "No concurrent clients");
    CHECK_STR_EQ(details.front().lifetime, "Survives client disconnect");
    CHECK(Runtime::hostSessionDetails(tree, { ObjectKind::Pane, 1 }).empty());
    CHECK(Runtime::hostSessionDetails(tree, { ObjectKind::Client, 5 }).empty());
    held.state = Session::TreeWorkspaceState::Restorable;
    CHECK(Runtime::hostSessionDetails(tree, { ObjectKind::Workspace, 1 }).empty());
}

TEST(aTerminateThatDidNotEndTheSessionSaysWhyAndWhetherToAskAgain)
{
    const auto notice = [](Session::TerminateOutcome outcome, const std::string& name) { return Runtime::terminateOutcomeNotice(std::to_underlying(outcome), name, 12); };
    // Terminated: the row goes, and nothing is said.
    const Runtime::SessionListNotice ended = notice(Session::TerminateOutcome::Terminated, "build");
    CHECK(ended.title.empty() && ended.message.empty() && !ended.doesOfferAskingAgain);
    const Runtime::SessionListNotice denied = notice(Session::TerminateOutcome::Denied, "build");
    CHECK_STR_EQ(denied.title, "A person on build declined.");
    CHECK_STR_EQ(denied.message, "The workspace is still running. Whoever is attached to it answered the host’s question with no.");
    CHECK(denied.doesOfferAskingAgain);
    const Runtime::SessionListNotice quiet = notice(Session::TerminateOutcome::TimedOut, "");
    CHECK_STR_EQ(quiet.title, "Nobody answered for session 12 in time.");
    CHECK(quiet.doesOfferAskingAgain && !quiet.message.empty());
    const Runtime::SessionListNotice unsent = notice(Session::TerminateOutcome::Failed, "build");
    CHECK_STR_EQ(unsent.title, "The request to end build didn’t reach the host.");
    CHECK_STR_EQ(unsent.message, "Nothing on the host was touched, and the workspace is still running. Check the connection and try again.");
    CHECK(!unsent.doesOfferAskingAgain);
    const Runtime::SessionListNotice refused = notice(Session::TerminateOutcome::Refused, "build");
    CHECK_STR_EQ(refused.title, "The host refused to terminate build.");
    CHECK(!refused.doesOfferAskingAgain && !refused.message.empty());
    // A number this build has no outcome for assumes nothing.
    const Runtime::SessionListNotice unknown = Runtime::terminateOutcomeNotice(200, "build", 12);
    CHECK_STR_EQ(unknown.title, "The host sent an answer about build that this version of Ubiquitty doesn’t understand.");
    CHECK_STR_EQ(unknown.message, "Nothing was assumed about the workspace. Refresh the list to see what the host is holding now.");
    CHECK(!unknown.doesOfferAskingAgain);

    const Runtime::SessionListNotice waiting = Runtime::listingFailedWhileConnectingNotice("devbox", "The host could not be reached.");
    CHECK_STR_EQ(waiting.title, "Ubiquitty couldn’t ask devbox which workspaces it’s holding.");
    CHECK_STR_EQ(waiting.message, "The host could not be reached.\n\nNothing was opened, so no workspace was started on the host. Check the connection and connect again.");
    CHECK_STR_EQ(Runtime::listingFailedWhileConnectingNotice("devbox", "").message, "Nothing was opened, so no workspace was started on the host. Check the connection and connect again.");
    const Runtime::SessionListNotice window = Runtime::windowNotOpenedNotice("devbox");
    CHECK_STR_EQ(window.title, "Ubiquitty couldn’t open a window on devbox.");
    CHECK_STR_EQ(window.message, "Nothing on the host was touched.");
}

TEST(aHostsSectionIsInOneStateDecidedFromWhatTheDirectoryReports)
{
    const auto state = [](HostSectionFacts facts) { return Runtime::hostSectionState(facts); };
    HostSectionFacts facts;
    // Nothing held: how far the asking has got.
    CHECK(state(facts) == HostSectionState::Asking);
    facts.progress = HostListingProgress::Asking;
    CHECK(state(facts) == HostSectionState::Asking);
    facts.progress = HostListingProgress::Listed;
    CHECK(state(facts) == HostSectionState::Empty);
    facts.progress = HostListingProgress::Failed;
    CHECK_MSG(state(facts) == HostSectionState::Unreachable, "a failure with no kind and nothing held is a host not reached");
    facts.workspaceCount = 2;
    CHECK(state(facts) == HostSectionState::Sessions);
    facts.progress = HostListingProgress::Asking;
    CHECK_MSG(state(facts) == HostSectionState::Sessions, "asking again over an earlier answer keeps it on screen");

    // A failed listing replaces what an earlier one left, unless the host
    // was only not reached.
    facts.progress = HostListingProgress::Failed;
    facts.failure = ListingFailure::Unreachable;
    CHECK(state(facts) == HostSectionState::Sessions);
    facts.workspaceCount = 0;
    CHECK(state(facts) == HostSectionState::Unreachable);
    for (const std::uint32_t held : { 0u, 3u }) {
        facts.workspaceCount = held;
        facts.failure = ListingFailure::SignInRequired;
        CHECK(state(facts) == HostSectionState::SignInRequired);
        facts.failure = ListingFailure::AuthenticationAttemptsExhausted;
        CHECK(state(facts) == HostSectionState::AuthenticationAttemptsExhausted);
        facts.failure = ListingFailure::EndedBeforeAnswer;
        CHECK(state(facts) == HostSectionState::EndedBeforeAnswer);
        facts.failure = ListingFailure::NotInstalled;
        CHECK(state(facts) == HostSectionState::NotInstalled);
        facts.failure = ListingFailure::Malformed;
        CHECK(state(facts) == HostSectionState::Malformed);
        facts.failure = ListingFailure::InstanceUnreadable;
        CHECK(state(facts) == HostSectionState::Malformed);
        facts.failure = ListingFailure::Refused;
        const std::pair<ByeReason, HostSectionState> refusals[] = {
            { ByeReason::TokenMissing, HostSectionState::NeedsToken },
            { ByeReason::TokenInvalid, HostSectionState::TokenInvalid },
            { ByeReason::TokenExpired, HostSectionState::TokenExpired },
            { ByeReason::TokenRevoked, HostSectionState::TokenRevoked },
            { ByeReason::OutsideScope, HostSectionState::TokenScope },
            { ByeReason::DuplicateHandshake, HostSectionState::Refused },
            { ByeReason::NoConsent, HostSectionState::Refused },
            { ByeReason::RemoteHostRefused, HostSectionState::Refused },
            { ByeReason::SessionGone, HostSectionState::Refused },
        };
        for (const auto& [reason, expected] : refusals) {
            facts.deniedReason = reason;
            CHECK(state(facts) == expected);
        }
        // A version refusal says which build is the older one, when the
        // two builds told.
        facts.deniedReason = ByeReason::VersionMismatch;
        facts.olderSide = OlderSide::Host;
        CHECK(state(facts) == HostSectionState::OlderHost);
        facts.olderSide = OlderSide::Client;
        CHECK(state(facts) == HostSectionState::OlderClient);
        facts.olderSide = OlderSide::Unknown;
        CHECK(state(facts) == HostSectionState::Refused);
    }
    // A host that cannot be dialled says so before anything else.
    facts.transportRefusal = Runtime::HostTransportRefusal::InvalidAddress;
    CHECK(state(facts) == HostSectionState::InvalidAddress);
    facts.transportRefusal = Runtime::HostTransportRefusal::MissingLocalHostBinary;
    CHECK(state(facts) == HostSectionState::HostBinaryMissing);
}

TEST(aHostsSectionHasALongAShortAndASpokenWordingAndAFactsLine)
{
    HostSectionFacts facts;
    facts.progress = HostListingProgress::Asking;
    HostSectionWords words = Runtime::hostSectionWords(facts, "devbox");
    CHECK(words.state == HostSectionState::Asking);
    CHECK_STR_EQ(words.title, "Asking devbox…");
    CHECK_STR_EQ(words.shortSentence, "Asking…");
    CHECK_STR_EQ(words.markWords, "Asking");
    CHECK_STR_EQ(words.subtitle, "Ubiquitty Host · Asking…");

    facts.progress = HostListingProgress::Listed;
    facts.isThisMachine = true;
    words = Runtime::hostSectionWords(facts, "This Mac");
    CHECK_STR_EQ(words.title, "No Workspaces on This Mac");
    CHECK_STR_EQ(words.shortSentence, "No workspaces.");
    CHECK(words.markWords.empty());
    CHECK_STR_EQ(words.subtitle, "This Mac · no workspaces");

    // A refusal is the host's own answer: the line says what it answered and
    // never that the host was not reached, whether or not it ever listed.
    {
        using Ubiquitty::Protocol::Sync::ByeReason;
        using Ubiquitty::Protocol::Sync::OlderSide;
        HostSectionFacts refused = facts;
        refused.progress = HostListingProgress::Failed;
        refused.failure = ListingFailure::Refused;
        refused.deniedReason = ByeReason::VersionMismatch;
        refused.olderSide = OlderSide::Host;
        CHECK_STR_EQ(Runtime::hostSectionWords(refused, "Ubiquitty Host").subtitle, "This Mac · runs an older host");
        refused.hasAnswered = true;
        refused.secondsSinceAnswered = 300;
        CHECK_STR_EQ(Runtime::hostSectionWords(refused, "Ubiquitty Host").subtitle, "This Mac · runs an older host");
        refused.olderSide = OlderSide::Client;
        CHECK_STR_EQ(Runtime::hostSectionWords(refused, "Ubiquitty Host").subtitle, "This Mac · runs a newer host");
        refused.deniedReason = ByeReason::TokenMissing;
        CHECK_STR_EQ(Runtime::hostSectionWords(refused, "Ubiquitty Host").subtitle, "This Mac · needs an access token");
        refused.deniedReason = ByeReason::NoConsent;
        CHECK_STR_EQ(Runtime::hostSectionWords(refused, "Ubiquitty Host").subtitle, "This Mac · refused the connection");
    }

    // Sessions a watch follows say nothing more; the facts line counts them.
    facts.isThisMachine = false;
    facts.workspaceCount = 3;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK(words.state == HostSectionState::Sessions && words.title.empty() && words.shortSentence.empty() && words.markWords.empty());
    CHECK_STR_EQ(words.subtitle, "Ubiquitty Host · 3 workspaces");
    facts.workspaceCount = 1;
    CHECK_STR_EQ(Runtime::hostSectionWords(facts, "devbox").subtitle, "Ubiquitty Host · 1 workspace");
    // Still on screen after the host went quiet: how old they are.
    facts.isStale = true;
    facts.hasAnswered = true;
    facts.secondsSinceAnswered = 300;
    CHECK_STR_EQ(Runtime::hostSectionWords(facts, "devbox").shortSentence, "Not reached. Last answered 5m ago.");
    facts.isStale = false;
    facts.progress = HostListingProgress::Failed;
    facts.failure = ListingFailure::Unreachable;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK(words.state == HostSectionState::Sessions);
    CHECK_STR_EQ(words.shortSentence, "Not reached. Last answered 5m ago.");
    CHECK_STR_EQ(words.subtitle, "Ubiquitty Host · last answered 5m ago");

    // Not reached, with nothing held.
    facts.workspaceCount = 0;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK_STR_EQ(words.title, "Can’t Reach devbox");
    CHECK_STR_EQ(words.shortSentence, "Can’t reach this host. Last answered 5m ago.");
    CHECK_STR_EQ(words.markWords, "Can’t reach this host");
    facts.hasAnswered = false;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK_STR_EQ(words.shortSentence, "Can’t reach this host.");
    CHECK_STR_EQ(words.subtitle, "Ubiquitty Host · not reached");

    facts.failure = ListingFailure::NotInstalled;
    facts.remotePlatform = "Linux x86_64";
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK_STR_EQ(words.title, "Ubiquitty Isn’t Installed on devbox");
    CHECK_STR_EQ(words.shortSentence, "Ubiquitty isn’t installed here.");
    CHECK_STR_EQ(words.markWords, "Not installed");
    CHECK_STR_EQ(words.subtitle, "Ubiquitty Host · not installed · Linux x86_64");
    facts.remotePlatform.clear();
    CHECK_STR_EQ(Runtime::hostSectionWords(facts, "devbox").subtitle, "Ubiquitty Host · not installed");

    facts.failure = ListingFailure::SignInRequired;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK_STR_EQ(words.title, "devbox Needs a Password or Key");
    CHECK_STR_EQ(words.shortSentence, "Needs a password or key.");
    CHECK_STR_EQ(words.markWords, "Can’t sign in");
    // A server that cut the sign-in short is named for that, never as a
    // host that could not be reached.
    facts.failure = ListingFailure::AuthenticationAttemptsExhausted;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK_STR_EQ(words.title, "Too Many Authentication Attempts");
    CHECK_STR_EQ(words.shortSentence, "Too many authentication attempts.");
    CHECK_STR_EQ(words.markWords, "Can’t sign in");
    facts.failure = ListingFailure::EndedBeforeAnswer;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK_STR_EQ(words.title, "devbox Closed the Connection");
    CHECK_STR_EQ(words.shortSentence, "Closed the connection before answering.");
    CHECK_STR_EQ(words.markWords, "Closed the connection");
    facts.failure = ListingFailure::Malformed;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK_STR_EQ(words.title, "devbox Sent an Unexpected Reply");
    CHECK_STR_EQ(words.shortSentence, "Sent a reply Ubiquitty doesn’t understand.");
    facts.failure = ListingFailure::Refused;
    const std::tuple<ByeReason, const char*, const char*, const char*> refusals[] = {
        { ByeReason::TokenMissing, "devbox Requires an Access Token", "Needs an access token.", "Needs an access token" },
        { ByeReason::TokenInvalid, "devbox Requires an Access Token", "Needs an access token.", "Needs an access token" },
        { ByeReason::TokenExpired, "devbox’s Access Token Has Expired", "The access token has expired.", "Needs an access token" },
        { ByeReason::TokenRevoked, "devbox’s Access Token Was Revoked", "The access token was revoked.", "Needs an access token" },
        { ByeReason::OutsideScope, "This Token Can’t Open Workspaces on devbox", "This token can’t open workspaces.", "Needs an access token" },
        { ByeReason::NoConsent, "devbox Refused the Connection", "Refused the connection.", "Can’t reach this host" },
    };
    for (const auto& [reason, title, shortSentence, mark] : refusals) {
        facts.deniedReason = reason;
        words = Runtime::hostSectionWords(facts, "devbox");
        CHECK_STR_EQ(words.title, title);
        CHECK_STR_EQ(words.shortSentence, shortSentence);
        CHECK_STR_EQ(words.markWords, mark);
    }
    // A version refusal's title is worded from the two builds elsewhere.
    facts.deniedReason = ByeReason::VersionMismatch;
    facts.olderSide = OlderSide::Host;
    words = Runtime::hostSectionWords(facts, "devbox");
    CHECK(words.title.empty());
    CHECK_STR_EQ(words.shortSentence, "Runs an older host.");
    CHECK_STR_EQ(words.markWords, "Runs an older host");
    facts.olderSide = OlderSide::Client;
    CHECK_STR_EQ(Runtime::hostSectionWords(facts, "devbox").shortSentence, "Runs a newer host than this app.");

    facts.transportRefusal = Runtime::HostTransportRefusal::InvalidAddress;
    CHECK_STR_EQ(Runtime::hostSectionWords(facts, "devbox").title, "devbox’s Address Isn’t Valid");
    facts.transportRefusal = Runtime::HostTransportRefusal::MissingLocalHostBinary;
    words = Runtime::hostSectionWords(facts, "This Mac");
    CHECK_STR_EQ(words.title, "Ubiquitty’s Host Is Missing");
    CHECK_STR_EQ(words.shortSentence, "This copy of Ubiquitty doesn’t include ubiquitty-host, so it can’t start a host here.");

    // Every state but the sessions has a short sentence, and every state
    // that is a failure has words for its mark.
    for (std::uint8_t reason = 0; reason <= std::to_underlying(Protocol::Sync::lastByeReasonOnWire); ++reason) {
        HostSectionFacts each;
        each.progress = HostListingProgress::Failed;
        each.failure = ListingFailure::Refused;
        each.deniedReason = static_cast<ByeReason>(reason);
        const HostSectionWords said = Runtime::hostSectionWords(each, "devbox");
        CHECK(!said.shortSentence.empty() && !said.markWords.empty() && !said.subtitle.empty());
    }
}

TEST(theDefaultButtonOfAConnectIsTitledFromThePlan)
{
    const auto said = [](const ConnectActionFacts& facts) { return Runtime::connectAction(facts); };
    const auto is = [](const ConnectAction& action, const char* title, const char* sentence, ConnectActionEffect effect, bool isEnabled) { // bool-ok: what the case expects
        CHECK_STR_EQ(action.title, title);
        CHECK_STR_EQ(action.sentence, sentence);
        CHECK(action.effect == effect);
        CHECK(action.isEnabled == isEnabled);
    };
    // No host, or no decision yet.
    ConnectActionFacts facts;
    is(said(facts), "Connect", "", ConnectActionEffect::None, false);
    facts.hostName = "devbox";
    is(said(facts), "Connect", "", ConnectActionEffect::None, false);
    facts.isDecided = true;

    // The plans.
    facts.plan = AttachPlan::AwaitListing;
    is(said(facts), "Connect", "Connects once devbox says which workspaces it holds.", ConnectActionEffect::Connect, true);
    CHECK(said(facts).plan == AttachPlan::AwaitListing);
    facts.plan = AttachPlan::CreateNew;
    is(said(facts), "New Workspace", "A workspace started here keeps running after you close its window.", ConnectActionEffect::Connect, true);
    facts.plan = AttachPlan::CreateNamed;
    facts.decidedWorkspaceName = "ci";
    is(said(facts), "Create “ci”", "Starts a workspace named “ci” on devbox.", ConnectActionEffect::Connect, true);
    facts.plan = AttachPlan::AttachNamed;
    is(said(facts), "Open “ci”", "Opens “ci”, the workspace devbox always opens.", ConnectActionEffect::Connect, true);
    CHECK(said(facts).plan == AttachPlan::AttachNamed);
    // The question is the sentence, and with none there is nothing to open.
    facts.plan = AttachPlan::ChooseFromList;
    is(said(facts), "Open", "", ConnectActionEffect::Connect, false);
    facts.questionText = "This host holds 2 workspaces, and nothing says which you were in. Which one?";
    is(said(facts), "Open", "This host holds 2 workspaces, and nothing says which you were in. Which one?", ConnectActionEffect::Connect, true);

    // The remembered session: reopened, by the name it has or the one its
    // row is called, or by none.
    facts.plan = AttachPlan::AttachByID;
    facts.decidedSessionID = 7;
    facts.decidedWorkspaceID = 3;
    is(said(facts), "Reopen “ci”", "Reopens “ci”, the workspace you last used on devbox.", ConnectActionEffect::Connect, true);
    facts.decidedWorkspaceName.clear();
    facts.landingRowTitle = "make all";
    is(said(facts), "Reopen “make all”", "Reopens “make all”, the workspace you last used on devbox.", ConnectActionEffect::Connect, true);
    facts.landingRowTitle.clear();
    is(said(facts), "Reopen Workspace", "Reopens the workspace you last used on devbox.", ConnectActionEffect::Connect, true);
    // The selected row is what it lands on: opened, with who is there.
    facts.isRowSelected = true;
    facts.selectedOpen.opening = AttachOpening::BySession;
    facts.selectedOpen.sessionID = 7;
    is(said(facts), "Open Workspace", "Opens this workspace.", ConnectActionEffect::Connect, true);
    facts.selectedAttachedPeople = 1;
    is(said(facts), "Open Workspace", "Opens this workspace. 1 person is attached.", ConnectActionEffect::Connect, true);
    facts.selectedAttachedPeople = 3;
    facts.landingRowTitle = "make all";
    is(said(facts), "Open “make all”", "Opens “make all”. 3 people are attached.", ConnectActionEffect::Connect, true);
    // A selected row of another session is not what the plan lands on.
    facts.selectedOpen.sessionID = 8;
    is(said(facts), "Reopen “make all”", "Reopens “make all”, the workspace you last used on devbox.", ConnectActionEffect::Connect, true);

    // A selected row that opens no session says what it does.
    facts.selectedOpen.opening = AttachOpening::ByRestoring;
    facts.selectedOpen.workspaceName = "kept";
    is(said(facts), "Open “kept”", "Opens “kept”.", ConnectActionEffect::Connect, true);
    CHECK(said(facts).plan == AttachPlan::AttachNamed);
    facts.selectedOpen.opening = AttachOpening::Cannot;
    facts.selectedOpen.obstacle = Session::AttachObstacle::PartOfARecord;
    is(said(facts), "Open", std::string(Session::attachObstacleSentence(Session::AttachObstacle::PartOfARecord)).c_str(), ConnectActionEffect::None, false);
    facts.selectedOpen.opening = AttachOpening::ByRevealing;
    is(said(facts), "Show Window", "Brings forward the window showing “kept”.", ConnectActionEffect::Reveal, true);
    facts.selectedOpen.workspaceName.clear();
    is(said(facts), "Show Window", "Brings forward the window showing the workspace.", ConnectActionEffect::Reveal, true);
    // What the plan lands in is already shown in a window here.
    facts.isRowSelected = false;
    facts.isLandingShownHere = true;
    facts.decidedWorkspaceName = "ci";
    is(said(facts), "Show Window", "Brings forward the window showing “ci”.", ConnectActionEffect::Reveal, true);
    // A remembered window needs no displayed workspace row. A new-session
    // plan must never reuse it merely because its saved name also matches.
    facts.landingRowTitle.clear();
    facts.decidedWorkspaceID = 0;
    facts.plan = AttachPlan::AttachByID;
    CHECK(said(facts).effect == ConnectActionEffect::Reveal);
    facts.plan = AttachPlan::AttachNamed;
    CHECK(said(facts).effect == ConnectActionEffect::Reveal);
    for (const AttachPlan plan : { AttachPlan::CreateNamed, AttachPlan::CreateNew, AttachPlan::ChooseFromList, AttachPlan::AwaitListing }) {
        facts.plan = plan;
        CHECK(said(facts).effect != ConnectActionEffect::Reveal);
    }
    // A connect waiting for the host comes before all of it.
    facts.isConnectPending = true;
    is(said(facts), "Waiting for devbox…", "Connects once devbox says which workspaces it holds. Press Esc to stop waiting.", ConnectActionEffect::Waiting, false);
}

TEST(thePlanMarksTheRowOfTheWorkspaceItLandsIn)
{
    using Runtime::ConnectRowMarker;
    ConnectActionFacts facts;
    facts.hostName = "devbox";
    facts.isDecided = true;
    facts.plan = AttachPlan::AttachByID;
    facts.decidedWorkspaceID = 3;
    CHECK(Runtime::connectRowMarker(facts, 3) == ConnectRowMarker::LastUsed);
    CHECK(Runtime::connectRowMarker(facts, 4) == ConnectRowMarker::None);
    CHECK_MSG(Runtime::connectRowMarker(facts, 0) == ConnectRowMarker::None, "a row of another level is no workspace");
    // A selection is what the button opens: nothing is "last used" then.
    facts.isRowSelected = true;
    CHECK(Runtime::connectRowMarker(facts, 3) == ConnectRowMarker::None);
    facts.plan = AttachPlan::AttachNamed;
    CHECK(Runtime::connectRowMarker(facts, 3) == ConnectRowMarker::AlwaysOpens);
    facts.isRowSelected = false;
    CHECK(Runtime::connectRowMarker(facts, 3) == ConnectRowMarker::AlwaysOpens);
    for (const AttachPlan plan : { AttachPlan::CreateNamed, AttachPlan::CreateNew, AttachPlan::ChooseFromList, AttachPlan::AwaitListing }) {
        facts.plan = plan;
        CHECK(Runtime::connectRowMarker(facts, 3) == ConnectRowMarker::None);
    }
    // No decision, or one that lands in no workspace, marks nothing.
    facts.plan = AttachPlan::AttachNamed;
    facts.decidedWorkspaceID = 0;
    CHECK(Runtime::connectRowMarker(facts, 0) == ConnectRowMarker::None);
    facts.decidedWorkspaceID = 3;
    facts.isDecided = false;
    CHECK(Runtime::connectRowMarker(facts, 3) == ConnectRowMarker::None);
}

UBIQUITTY_TEST_MAIN()
