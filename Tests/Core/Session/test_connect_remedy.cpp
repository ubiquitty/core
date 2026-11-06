// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_connect_remedy.cpp — the ordered remedies a connect that did not land
// offers (Session/ConnectRemedy.h). The list is a decision: its first entry is
// what Return does, so each case asserts the whole order, not membership. The
// negatives are the answers that would hurt: an update offered when this client
// is the older end, Close Window as the default where a recovery exists, and a
// refused named session reopened unnamed.

#include "Core/Session/ConnectRemedy.h"

#include <algorithm>
#include <vector>

using Ubiquitty::Protocol::Sync::ByeReason;
using Ubiquitty::Session::ConnectFact;
using Ubiquitty::Session::connectRemedies;
using Ubiquitty::Session::connectRemedySentence;
using Ubiquitty::Session::DeployAction;
using Ubiquitty::Session::ListingFailure;
using Ubiquitty::Session::Remedy;
using Ubiquitty::Session::RemedySurface;

namespace {

ConnectFact listingFailed(ListingFailure failure)
{
    ConnectFact fact;
    fact.kind = ConnectFact::Kind::ListingFailed;
    fact.listingFailure = failure;
    return fact;
}

ConnectFact refused(ByeReason reason, RemedySurface surface)
{
    ConnectFact fact;
    fact.kind = ConnectFact::Kind::AttachRefused;
    fact.surface = surface;
    fact.deniedReason = reason;
    return fact;
}

bool contains(const std::vector<Remedy>& remedies, Remedy remedy)
{
    return std::ranges::find(remedies, remedy) != remedies.end();
}

} // namespace

// Every listing failure names its own way forward, default first.
TEST(eachListingFailureOffersItsRemedyFirst)
{
    CHECK(connectRemedies(listingFailed(ListingFailure::Unreachable)) == (std::vector { Remedy::TryAgain, Remedy::EditAddress }));
    CHECK(connectRemedies(listingFailed(ListingFailure::SignInRequired)) == (std::vector { Remedy::OpenSSHShell, Remedy::TryAgain }));
    CHECK(connectRemedies(listingFailed(ListingFailure::Malformed)) == (std::vector { Remedy::TryAgain }));
    CHECK(connectRemedies(listingFailed(ListingFailure::None)).empty());
    // A shell would offer the same keys and meet the same limit.
    CHECK(connectRemedies(listingFailed(ListingFailure::AuthenticationAttemptsExhausted)) == (std::vector { Remedy::TryAgain }));
    CHECK(connectRemedies(listingFailed(ListingFailure::EndedBeforeAnswer)) == (std::vector { Remedy::OpenSSHShell, Remedy::TryAgain }));

    ConnectFact token = listingFailed(ListingFailure::Refused);
    token.deniedReason = ByeReason::TokenMissing;
    CHECK(connectRemedies(token) == (std::vector { Remedy::EnterToken, Remedy::ManageHost }));
}

// What SSH said is the sentence: a sign-in the server cut short names the
// attempts and the configuration that ends them, and a silent end never
// blames reachability. No sentence spells the protocol in lowercase.
TEST(aSignInFailureIsWordedAsTheServersAnswerNotAsAnUnreachableHost)
{
    CHECK_STR_EQ(connectRemedySentence(listingFailed(ListingFailure::AuthenticationAttemptsExhausted)),
        "The server closed the connection after too many authentication attempts. Set IdentityFile and IdentitiesOnly for this host in ~/.ssh/config so SSH offers only its key.");
    CHECK_STR_EQ(connectRemedySentence(listingFailed(ListingFailure::EndedBeforeAnswer)),
        "The host closed the SSH connection before Ubiquitty’s host answered. Open an SSH shell to see why ubiquitty-host didn’t start.");
    CHECK_STR_EQ(connectRemedySentence(listingFailed(ListingFailure::SignInRequired)),
        "SSH needs a password or an unlocked key, and it can’t ask for one here. Open an SSH shell, where SSH can ask.");
    CHECK_STR_EQ(connectRemedySentence(listingFailed(ListingFailure::Unreachable)),
        "SSH couldn’t reach the host. Check its address and your network, then try again.");
    for (const auto failure : { ListingFailure::Unreachable, ListingFailure::SignInRequired, ListingFailure::NotInstalled,
             ListingFailure::AuthenticationAttemptsExhausted, ListingFailure::EndedBeforeAnswer }) {
        ConnectFact fact = listingFailed(failure);
        fact.deployAction = DeployAction::Push;
        const std::string sentence = connectRemedySentence(fact);
        CHECK_MSG(sentence.find("ssh ") == std::string::npos && sentence.find("ssh’") == std::string::npos && !sentence.starts_with("ssh"),
            "an interface sentence spells SSH in capitals");
        CHECK_MSG(sentence.find("transport") == std::string::npos, "the channel's words never reach the person");
        CHECK_MSG(!sentence.empty() && sentence.back() == '.', "the sentence is a complete one");
    }
}

// A connection that never opened: the host's own answer to what was typed
// sends the person to the address first, and a failure a path still coming up
// also reports offers another try first. Each says what to do in a sentence
// of its own, beside Transport's sentence of what happened.
TEST(aConnectionThatNeverOpenedOffersTheAddressOrAnotherTryFirst)
{
    using Ubiquitty::Transport::ConnectFailure;
    const auto failed = [](ConnectFailure failure) {
        ConnectFact fact;
        fact.kind = ConnectFact::Kind::ConnectFailed;
        fact.connectFailure = failure;
        return fact;
    };
    CHECK(connectRemedies(failed(ConnectFailure::Refused)) == (std::vector { Remedy::EditAddress, Remedy::TryAgain }));
    CHECK(connectRemedies(failed(ConnectFailure::NameNotResolved)) == (std::vector { Remedy::EditAddress, Remedy::TryAgain }));
    CHECK(connectRemedies(failed(ConnectFailure::TimedOut)) == (std::vector { Remedy::TryAgain, Remedy::EditAddress }));
    CHECK(connectRemedies(failed(ConnectFailure::NetworkUnreachable)) == (std::vector { Remedy::TryAgain }));
    CHECK_STR_EQ(connectRemedySentence(failed(ConnectFailure::Refused)), "Check the host’s port, then try again.");
    ConnectFact onWindow = failed(ConnectFailure::Refused);
    onWindow.surface = RemedySurface::TerminalWindow;
    CHECK(connectRemedies(onWindow).back() == Remedy::CloseWindow);
    for (const auto failure : { ConnectFailure::Refused, ConnectFailure::TimedOut, ConnectFailure::Reset, ConnectFailure::HostUnreachable,
             ConnectFailure::NetworkUnreachable, ConnectFailure::NameNotResolved, ConnectFailure::Other }) {
        const std::string sentence = connectRemedySentence(failed(failure));
        CHECK_MSG(!sentence.empty() && sentence.back() == '.', "the sentence is a complete one");
        CHECK_MSG(sentence != Ubiquitty::Transport::describeConnectFailure(failure, "host.example", 22),
            "the remedy repeats what happened instead of saying what to do");
    }
}

TEST(aListingRefusalWithoutAnAuthenticationVerdictOffersNoTokenRemedy)
{
    const Ubiquitty::Session::DirectoryFailureReport report;
    CHECK(report.deniedReason == ByeReason::Normal);
    ConnectFact fact = listingFailed(ListingFailure::Refused);
    fact.deniedReason = report.deniedReason;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::ManageHost, Remedy::TryAgain }));
    CHECK(!contains(connectRemedies(fact), Remedy::EnterToken));
    CHECK_STR_EQ(connectRemedySentence(fact), "The host refused to list its workspaces for this connection.");
}

// A host with nothing installed offers what the deploy decision allows. Under
// ConfirmPush the install is the default: the person reading the consent text
// on screen and pressing Return is the trust-on-first-use consent.
TEST(aHostWithNothingInstalledOffersWhatTheDeployDecisionAllows)
{
    ConnectFact fact = listingFailed(ListingFailure::NotInstalled);
    fact.deployAction = DeployAction::ConfirmPush;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::InstallAndConnect, Remedy::CopyInstallCommands }));
    fact.deployAction = DeployAction::Push;
    CHECK(connectRemedies(fact).front() == Remedy::InstallAndConnect);
    fact.deployAction = DeployAction::ManualInstall;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::CopyInstallCommands, Remedy::TryAgain }));
    CHECK(!contains(connectRemedies(fact), Remedy::InstallAndConnect));
    fact.deployAction = DeployAction::Off;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::OpenSettings, Remedy::CopyInstallCommands }));
    CHECK(!contains(connectRemedies(fact), Remedy::InstallAndConnect));
    CHECK(!connectRemedySentence(fact).empty());
}

// A host on a platform no release publishes a host for is offered no commands,
// because there are none to run, and the sentence says so plainly. A push from
// a build that bundles that platform is still offered. A published platform,
// and a host that did not name one, keep the commands.
TEST(aPlatformNoReleaseServesIsOfferedNoInstallCommands)
{
    ConnectFact fact = listingFailed(ListingFailure::NotInstalled);
    fact.remotePlatform = "FreeBSD amd64";
    fact.deployAction = DeployAction::ManualInstall;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::TryAgain }));
    CHECK_STR_EQ(connectRemedySentence(fact), "There’s no prebuilt ubiquitty-host for FreeBSD amd64 yet.");
    fact.deployAction = DeployAction::Off;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::OpenSettings }));
    CHECK_STR_EQ(connectRemedySentence(fact), "There’s no prebuilt ubiquitty-host for FreeBSD amd64 yet.");
    fact.deployAction = DeployAction::ConfirmPush;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::InstallAndConnect }));
    CHECK(connectRemedySentence(fact).starts_with("Ubiquitty isn’t installed on"));

    ConnectFact older = refused(ByeReason::VersionMismatch, RemedySurface::TerminalWindow);
    older.remotePlatform = "FreeBSD amd64";
    CHECK(!contains(connectRemedies(older), Remedy::CopyInstallCommands));
    CHECK(contains(connectRemedies(older), Remedy::CloseWindow));

    for (const char* platform : { "Linux x86_64", "Linux aarch64", "Darwin arm64", "" }) {
        ConnectFact served = listingFailed(ListingFailure::NotInstalled);
        served.remotePlatform = platform;
        served.deployAction = DeployAction::ManualInstall;
        CHECK(connectRemedies(served) == (std::vector { Remedy::CopyInstallCommands, Remedy::TryAgain }));
        CHECK(connectRemedySentence(served).ends_with("by hand, then try again."));
    }
}

// Every refusal reason on a terminal window, in order. The window can always
// be closed, so Close Window is always present — and never first where
// anything else recovers.
TEST(eachRefusalReasonOffersItsRemediesInOrder)
{
    const auto onWindow = [](ByeReason reason) { return connectRemedies(refused(reason, RemedySurface::TerminalWindow)); };
    for (const ByeReason reason : { ByeReason::TokenMissing, ByeReason::TokenInvalid, ByeReason::TokenExpired, ByeReason::TokenRevoked })
        CHECK(onWindow(reason) == (std::vector { Remedy::EnterToken, Remedy::ManageHost, Remedy::CloseWindow }));
    CHECK(onWindow(ByeReason::OutsideScope) == (std::vector { Remedy::EnterToken, Remedy::ManageHost, Remedy::CloseWindow }));
    CHECK(onWindow(ByeReason::DuplicateHandshake) == (std::vector { Remedy::TryAgain, Remedy::CloseWindow }));
    CHECK(onWindow(ByeReason::NoConsent) == (std::vector { Remedy::AskAgain, Remedy::ChooseAnother, Remedy::CloseWindow }));
    CHECK(onWindow(ByeReason::RemoteHostRefused) == (std::vector { Remedy::EditAddress, Remedy::CloseWindow }));
    CHECK(onWindow(ByeReason::SessionGone) == (std::vector { Remedy::StartNewSession, Remedy::ChooseAnother, Remedy::CloseWindow }));

    for (const ByeReason reason : { ByeReason::TokenMissing, ByeReason::OutsideScope, ByeReason::DuplicateHandshake,
             ByeReason::NoConsent, ByeReason::RemoteHostRefused, ByeReason::SessionGone }) {
        const std::vector<Remedy> remedies = onWindow(reason);
        REQUIRE(!remedies.empty());
        CHECK(remedies.front() != Remedy::CloseWindow);
        CHECK(remedies.back() == Remedy::CloseWindow);
    }

    // Before a window exists there is none to close.
    CHECK(!contains(connectRemedies(refused(ByeReason::NoConsent, RemedySurface::Connect)), Remedy::CloseWindow));
}

// A session that ended under a name reopens under that name; an unnamed one
// opens a new unnamed session. Reopening a named one unnamed loses the name
// Connect returns to next time.
TEST(aSessionThatEndedUnderANameStartsNewUnderIt)
{
    ConnectFact fact = refused(ByeReason::SessionGone, RemedySurface::TerminalWindow);
    fact.hasWorkspaceName = true;
    const std::vector<Remedy> named = connectRemedies(fact);
    REQUIRE(!named.empty());
    CHECK(named.front() == Remedy::StartNewNamed);
    CHECK(!contains(named, Remedy::StartNewSession));
}

TEST(anOutOfScopeRequestOffersANewTokenWithoutClaimingAllSessionsAreForbidden)
{
    for (const RemedySurface surface : { RemedySurface::Connect, RemedySurface::TerminalWindow }) {
        const ConnectFact fact = refused(ByeReason::OutsideScope, surface);
        const std::vector<Remedy> expected = surface == RemedySurface::TerminalWindow
            ? std::vector { Remedy::EnterToken, Remedy::ManageHost, Remedy::CloseWindow }
            : std::vector { Remedy::EnterToken, Remedy::ManageHost };
        CHECK(connectRemedies(fact) == expected);
        CHECK_STR_EQ(connectRemedySentence(fact), "This access token doesn’t allow this request. Use a token with the required permissions.");
        CHECK(!contains(connectRemedies(fact), Remedy::TryAgain));
        CHECK(!contains(connectRemedies(fact), Remedy::StartNewSession));
        CHECK(!contains(connectRemedies(fact), Remedy::AskAgain));
    }
}

// An older host is offered its update with the sentence on held sessions, and
// so is a host that states no build — one from before the published wire; an
// older client never is, on either surface, because a push would overwrite a
// newer host with this older build. The sentence names the older end.
TEST(anOlderHostIsOfferedAnUpdateAndAnOlderClientNeverIs)
{
    const auto build = [](const char* appVersion, std::uint16_t version) {
        return Ubiquitty::Protocol::Sync::PeerBuild { .isKnown = true, .wireVersion = version, .appVersion = appVersion };
    };
    for (const RemedySurface surface : { RemedySurface::Connect, RemedySurface::TerminalWindow }) {
        ConnectFact host = refused(ByeReason::VersionMismatch, surface);
        host.update.local = build("0.2.1.4", 5);
        host.update.host = build("0.2.0.7", 3);
        host.update.hostLabel = "Studio";
        host.update.hasBundledBinary = true;
        host.update.heldSessionCount = 3;
        const std::vector<Remedy> hostRemedies = connectRemedies(host);
        REQUIRE(!hostRemedies.empty());
        CHECK(hostRemedies.front() == Remedy::UpdateHost);
        CHECK(connectRemedySentence(host).starts_with("Studio’s Ubiquitty Host is from an older build (0.2.0.7) than this device (0.2.1.4)."));
        CHECK(connectRemedySentence(host).find("3 running sessions move to the new host") != std::string::npos);

        ConnectFact client = refused(ByeReason::VersionMismatch, surface);
        client.update.local = build("0.2.0.7", 3);
        client.update.host = build("0.2.1.4", 5);
        client.update.hostLabel = "Studio";
        client.update.localDevice.kind = Ubiquitty::Session::LocalDeviceKind::Mac;
        client.update.hasBundledBinary = true;
        CHECK(!contains(connectRemedies(client), Remedy::UpdateHost));
        CHECK(connectRemedySentence(client)
            == "This Mac is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this Mac.");

        ConnectFact silent = refused(ByeReason::VersionMismatch, surface);
        silent.update.hasBundledBinary = true;
        REQUIRE(!connectRemedies(silent).empty());
        CHECK(connectRemedies(silent).front() == Remedy::UpdateHost);

        // At equal wire versions, app labels neither establish direction nor
        // hide the explicit choice to install this app's matching host.
        ConnectFact same = refused(ByeReason::VersionMismatch, surface);
        same.update.local = build("0.2.1.4", 5);
        same.update.host = build("0.2.2", 5);
        same.update.hasBundledBinary = true;
        same.update.hostLabel = "Studio";
        CHECK(olderSideOf(same.update) == Ubiquitty::Protocol::Sync::OlderSide::Unknown);
        CHECK(versionRefusalTitle(same.update) == "Studio Runs a Different Build of Ubiquitty");
        CHECK(contains(connectRemedies(same), Remedy::UpdateHost));
        CHECK(connectRemedySentence(same).find("Install the same build on both.") != std::string::npos);
        std::swap(same.update.local, same.update.host);
        CHECK(olderSideOf(same.update) == Ubiquitty::Protocol::Sync::OlderSide::Unknown);
        CHECK(contains(connectRemedies(same), Remedy::UpdateHost));
        same.update.isThisMachine = true;
        same.update.localDevice.kind = Ubiquitty::Session::LocalDeviceKind::Mac;
        CHECK(versionRefusalTitle(same.update) == "This Mac Runs a Different Build of the Ubiquitty Host");
        CHECK(contains(connectRemedies(same), Remedy::UpdateHost));

        // An older host with no build for its platform is shown the commands.
        ConnectFact manual = host;
        manual.update.hasBundledBinary = false;
        CHECK(!contains(connectRemedies(manual), Remedy::UpdateHost));
        CHECK(connectRemedies(manual).front() == Remedy::CopyInstallCommands);
        manual.remotePlatform = "Linux x86_64";
        CHECK(connectRemedies(manual).front() == Remedy::CopyInstallCommands);
        CHECK(connectRemedySentence(manual).find("by hand") != std::string::npos);

        // A platform no release publishes a host for has nothing to copy,
        // and the sentence sends nobody to install one by hand.
        manual.remotePlatform = "FreeBSD amd64";
        CHECK(!contains(connectRemedies(manual), Remedy::CopyInstallCommands));
        CHECK(connectRemedySentence(manual).ends_with(" There’s no prebuilt ubiquitty-host for FreeBSD amd64 yet."));
        CHECK(connectRemedySentence(manual).find("by hand") == std::string::npos);
    }
}

// A consent the host's people declined is asked again first.
TEST(aDeclinedConsentIsAskedAgainFirst)
{
    ConnectFact fact;
    fact.kind = ConnectFact::Kind::ConsentDenied;
    fact.surface = RemedySurface::TerminalWindow;
    CHECK(connectRemedies(fact) == (std::vector { Remedy::AskAgain, Remedy::ChooseAnother, Remedy::CloseWindow }));
}

// This Mac's older host is updated by the host this app bundles taking it
// over, on either surface; the sentence names the machine and the app and
// offers no commands to paste. A listing that fails before This Mac's host
// answers is tried again: there is no SSH to open and nothing to install.
TEST(thisMachinesOlderHostIsUpdatedInPlaceNeverByHand)
{
    const auto build = [](const char* appVersion, std::uint16_t version) {
        return Ubiquitty::Protocol::Sync::PeerBuild { .isKnown = true, .wireVersion = version, .appVersion = appVersion };
    };
    const auto local = [](ConnectFact fact) {
        fact.update.hostLabel = "Ubiquitty Host";
        fact.update.localDevice.kind = Ubiquitty::Session::LocalDeviceKind::Mac;
        fact.update.isThisMachine = true;
        fact.update.hasBundledBinary = true;
        return fact;
    };
    for (const RemedySurface surface : { RemedySurface::Connect, RemedySurface::TerminalWindow }) {
        ConnectFact older = local(refused(ByeReason::VersionMismatch, surface));
        older.update.local = build("0.2.1", 2);
        older.update.host = build("0.2.0", 1);
        older.update.heldSessionCount = 3;
        const std::vector<Remedy> remedies = connectRemedies(older);
        REQUIRE(!remedies.empty());
        CHECK(remedies.front() == Remedy::UpdateHost);
        CHECK(!contains(remedies, Remedy::CopyInstallCommands));
        CHECK_STR_EQ(connectRemedySentence(older),
            "The Ubiquitty host on this Mac is from an older build (0.2.0) than this app (0.2.1). "
            "Updating moves its sessions to the host included with this app without ending them.");

        // The same refusal met by the Connect window's listing.
        ConnectFact listing = local(listingFailed(ListingFailure::Refused));
        listing.surface = surface;
        listing.deniedReason = ByeReason::VersionMismatch;
        listing.update.local = older.update.local;
        listing.update.host = older.update.host;
        REQUIRE(!connectRemedies(listing).empty());
        CHECK(connectRemedies(listing).front() == Remedy::UpdateHost);
        CHECK(!contains(connectRemedies(listing), Remedy::CopyInstallCommands));

        // An older app beside a newer host on this Mac replaces nothing.
        ConnectFact newer = local(refused(ByeReason::VersionMismatch, surface));
        newer.update.local = build("0.2.0", 1);
        newer.update.host = build("0.2.1", 2);
        CHECK(!contains(connectRemedies(newer), Remedy::UpdateHost));
    }

    for (const auto failure : { ListingFailure::Unreachable, ListingFailure::SignInRequired, ListingFailure::AuthenticationAttemptsExhausted,
             ListingFailure::EndedBeforeAnswer, ListingFailure::NotInstalled }) {
        ConnectFact fact = local(listingFailed(failure));
        fact.deployAction = DeployAction::ManualInstall;
        CHECK(connectRemedies(fact) == (std::vector { Remedy::TryAgain }));
        CHECK_STR_EQ(connectRemedySentence(fact), "The Ubiquitty host on this Mac didn’t answer. Try again.");
    }
    CHECK_STR_EQ(connectRemedySentence(local(listingFailed(ListingFailure::InstanceGone))),
        "The Ubiquitty host you chose on this Mac is no longer running. Try again to see the sessions this Mac is running now.");
    CHECK_STR_EQ(connectRemedySentence(local(listingFailed(ListingFailure::Malformed))),
        "The Ubiquitty host on this Mac sent a reply this app doesn’t understand. Try again.");
    const auto unreadable = local(listingFailed(ListingFailure::InstanceUnreadable));
    CHECK(connectRemedies(unreadable) == (std::vector { Remedy::TryAgain }));
    CHECK_STR_EQ(connectRemedySentence(unreadable),
        "This app cannot read the running host. Its sessions keep running. Use a matching build to open them.");
    for (const auto failure : { ListingFailure::Unreachable, ListingFailure::SignInRequired, ListingFailure::AuthenticationAttemptsExhausted,
             ListingFailure::EndedBeforeAnswer, ListingFailure::NotInstalled, ListingFailure::InstanceGone, ListingFailure::Malformed }) {
        const std::string sentence = connectRemedySentence(local(listingFailed(failure)));
        CHECK(sentence.find("Ubiquitty Host") == std::string::npos);
        CHECK(sentence.find("SSH") == std::string::npos);
        CHECK(sentence.find("no host build") == std::string::npos);
        CHECK(sentence.find("by hand") == std::string::npos);
    }
}

TEST(aSameVersionRefusalOffersTheMatchingHost)
{
    for (const auto surface : { RemedySurface::Connect, RemedySurface::TerminalWindow }) {
        ConnectFact fact = refused(ByeReason::VersionMismatch, surface);
        fact.update.local = { .isKnown = true, .wireVersion = 20, .appVersion = "0.2.0" };
        fact.update.host = fact.update.local;
        fact.update.hasBundledBinary = true;
        CHECK(contains(connectRemedies(fact), Remedy::UpdateHost));
        fact.update.hasBundledBinary = false;
        CHECK(contains(connectRemedies(fact), Remedy::CopyInstallCommands));
    }
}

TEST(anUpdatedAppOffersToUpdateAWireOneHost)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    for (const auto surface : { RemedySurface::Connect, RemedySurface::TerminalWindow }) {
        ConnectFact fact = refused(ByeReason::VersionMismatch, surface);
        fact.update.local = { .isKnown = true, .wireVersion = Sync::wireVersion, .appVersion = "0.3.3" };
        fact.update.host = { .isKnown = true, .wireVersion = 1, .appVersion = "0.3.2" };
        fact.update.hostLabel = "Studio";
        fact.update.localDevice.kind = Ubiquitty::Session::LocalDeviceKind::IPhone;
        fact.update.heldSessionCount = 2;
        fact.update.hasBundledBinary = true;
        REQUIRE(!connectRemedies(fact).empty());
        CHECK(connectRemedies(fact).front() == Remedy::UpdateHost);
        CHECK_STR_EQ(versionRefusalTitle(fact.update), "Studio Runs an Older Ubiquitty Host");
        CHECK_STR_EQ(connectRemedySentence(fact),
            "Studio’s Ubiquitty Host is from an older build (0.3.2) than this iPhone (0.3.3). Update the host on Studio. "
            "Ubiquitty copies its current host there and connects to it. Its 2 running sessions move to the new host. "
            "If handover is refused, the old host keeps them running.");
    }
}

UBIQUITTY_TEST_MAIN()
