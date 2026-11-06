// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/ConnectRemedy.cpp — see ConnectRemedy.h.

#include "Core/Session/ConnectRemedy.h"

#include "Core/Protocol/Sync/AttachVerdictText.h"

#include <algorithm>

namespace Ubiquitty::Session {

namespace {

/// Whether the host named a platform no release publishes a host for, so no
/// commands can install one by hand.
///
/// A host that did not name its platform keeps the commands, which then say
/// for themselves that no published host matches.
///
/// - Parameter fact: What happened.
/// - Returns: Whether the platform is known and unserved.
bool isUnpublishedPlatform(const ConnectFact& fact)
{
    return !fact.remotePlatform.empty() && publishedHostAsset(fact.remotePlatform).empty();
}

/// The remedies for a refused handshake, before the surface is applied.
///
/// - Parameter fact: What happened.
/// - Returns: The remedies, default first.
std::vector<Remedy> refusalRemedies(const ConnectFact& fact)
{
    using Protocol::Sync::ByeReason;
    switch (fact.deniedReason) {
    case ByeReason::TokenMissing:
    case ByeReason::TokenInvalid:
    case ByeReason::TokenExpired:
    case ByeReason::TokenRevoked:
        return { Remedy::EnterToken, Remedy::ManageHost, Remedy::CloseWindow };
    case ByeReason::OutsideScope:
        return { Remedy::EnterToken, Remedy::ManageHost, Remedy::CloseWindow };
    case ByeReason::VersionMismatch:
        switch (decideUpdateAction(fact.update)) {
        case UpdateAction::OfferHostUpdate:
        case UpdateAction::TakeOverLocalHost:
            return { Remedy::UpdateHost, Remedy::CloseWindow };
        case UpdateAction::ManualUpdate:
            return { Remedy::CopyInstallCommands, Remedy::CloseWindow };
        case UpdateAction::None:
            break;
        }
        // Replacing a known newer host would downgrade it.
        return { Remedy::CloseWindow };
    case ByeReason::DuplicateHandshake:
    // A full host has room once a session on it ends.
    case ByeReason::HostFull:
        return { Remedy::TryAgain, Remedy::CloseWindow };
    case ByeReason::NoConsent:
        return { Remedy::AskAgain, Remedy::ChooseAnother, Remedy::CloseWindow };
    case ByeReason::RemoteHostRefused:
        return { Remedy::EditAddress, Remedy::CloseWindow };
    case ByeReason::SessionGone:
        return { fact.hasWorkspaceName ? Remedy::StartNewNamed : Remedy::StartNewSession, Remedy::ChooseAnother, Remedy::CloseWindow };
    // Not refusals: a connection that ended or failed has no remedy of its
    // own here.
    case ByeReason::Normal:
    case ByeReason::Malformed:
    case ByeReason::Oversize:
    case ByeReason::UnknownKind:
    case ByeReason::UnboundHandle:
    case ByeReason::TransferRefused:
    case ByeReason::Superseded:
    case ByeReason::ResumeInProgress:
    case ByeReason::Suspending:
    case ByeReason::HandingOver:
    case ByeReason::OutputBacklog:
    case ByeReason::Silent:
        break;
    }
    return { Remedy::CloseWindow };
}

/// The remedies for a failed listing, before the surface is applied.
///
/// - Parameter fact: What happened.
/// - Returns: The remedies, default first.
std::vector<Remedy> listingRemedies(const ConnectFact& fact)
{
    // This machine's host is reached without SSH and is never installed by
    // hand: what failed before it answered is tried again.
    if (fact.update.isThisMachine) {
        switch (fact.listingFailure) {
        case ListingFailure::Unreachable:
        case ListingFailure::SignInRequired:
        case ListingFailure::AuthenticationAttemptsExhausted:
        case ListingFailure::EndedBeforeAnswer:
        case ListingFailure::NotInstalled:
            return { Remedy::TryAgain };
        case ListingFailure::None:
        case ListingFailure::InstanceGone:
        case ListingFailure::InstanceUnreadable:
        case ListingFailure::Refused:
        case ListingFailure::Malformed:
            break;
        }
    }
    switch (fact.listingFailure) {
    case ListingFailure::None:
        return {};
    case ListingFailure::Unreachable:
        return { Remedy::TryAgain, Remedy::EditAddress };
    case ListingFailure::SignInRequired:
        return { Remedy::OpenSSHShell, Remedy::TryAgain };
    // An interactive shell offers the same keys and meets the same limit, so
    // the remedy is the person's SSH configuration, then another try.
    case ListingFailure::AuthenticationAttemptsExhausted:
        return { Remedy::TryAgain };
    // The shell is where the person sees why the host's command ended.
    case ListingFailure::EndedBeforeAnswer:
        return { Remedy::OpenSSHShell, Remedy::TryAgain };
    // Another try goes to the host's current daemon.
    case ListingFailure::InstanceGone:
        return { Remedy::TryAgain };
    case ListingFailure::NotInstalled:
        switch (fact.deployAction) {
        case DeployAction::Push:
        case DeployAction::ConfirmPush:
            return { Remedy::InstallAndConnect, Remedy::CopyInstallCommands };
        case DeployAction::ManualInstall:
            return { Remedy::CopyInstallCommands, Remedy::TryAgain };
        case DeployAction::Off:
            return { Remedy::OpenSettings, Remedy::CopyInstallCommands };
        }
        return { Remedy::CopyInstallCommands };
    case ListingFailure::Refused:
        if (fact.deniedReason == Protocol::Sync::ByeReason::Normal)
            return { Remedy::ManageHost, Remedy::TryAgain };
        return refusalRemedies(fact);
    case ListingFailure::Malformed:
    case ListingFailure::InstanceUnreadable:
        return { Remedy::TryAgain };
    }
    return { Remedy::TryAgain };
}

/// The remedies for a connection that never opened, before the surface is
/// applied.
///
/// A refusal and a name that does not resolve are the host's own answer to
/// what was typed, so correcting the address comes first; every other failure
/// is also what a path still coming up reports, so another try comes first.
///
/// - Parameter failure: What the connect reported.
/// - Returns: The remedies, default first.
std::vector<Remedy> connectFailureRemedies(Transport::ConnectFailure failure)
{
    using Transport::ConnectFailure;
    switch (failure) {
    case ConnectFailure::Refused:
    case ConnectFailure::NameNotResolved:
        return { Remedy::EditAddress, Remedy::TryAgain };
    case ConnectFailure::TimedOut:
    case ConnectFailure::Reset:
    case ConnectFailure::HostUnreachable:
        return { Remedy::TryAgain, Remedy::EditAddress };
    case ConnectFailure::NetworkUnreachable:
    case ConnectFailure::Other:
        return { Remedy::TryAgain };
    }
    return { Remedy::TryAgain };
}

/// What to do about a connection that never opened, beside
/// ``Transport::describeConnectFailure``'s sentence of what happened.
///
/// - Parameter failure: What the connect reported.
/// - Returns: The sentence, verbatim.
std::string connectFailureSentence(Transport::ConnectFailure failure)
{
    using Transport::ConnectFailure;
    switch (failure) {
    case ConnectFailure::Refused:
        return "Check the host’s port, then try again.";
    case ConnectFailure::TimedOut:
        return "Check the host’s address and your network, then try again.";
    case ConnectFailure::Reset:
        return "The host may be restarting or may not run SSH on that port. Try again in a moment.";
    case ConnectFailure::HostUnreachable:
        return "Check your network or VPN, then try again.";
    case ConnectFailure::NetworkUnreachable:
        return "Connect to a network that reaches the host, then try again.";
    case ConnectFailure::NameNotResolved:
        return "Check the spelling of the host’s address.";
    case ConnectFailure::Other:
        break;
    }
    return "Try again in a moment.";
}

} // namespace

std::vector<Remedy> connectRemedies(const ConnectFact& fact)
{
    std::vector<Remedy> remedies;
    switch (fact.kind) {
    case ConnectFact::Kind::ListingFailed:
        remedies = listingRemedies(fact);
        break;
    case ConnectFact::Kind::AttachRefused:
        remedies = refusalRemedies(fact);
        break;
    case ConnectFact::Kind::ConsentDenied:
        remedies = { Remedy::AskAgain, Remedy::ChooseAnother, Remedy::CloseWindow };
        break;
    case ConnectFact::Kind::ConnectFailed:
        remedies = connectFailureRemedies(fact.connectFailure);
        break;
    }
    if (isUnpublishedPlatform(fact))
        std::erase(remedies, Remedy::CopyInstallCommands);
    // Before a window exists there is none to close; on a window, closing it
    // is always possible, so the sheet always has a way out.
    if (fact.surface == RemedySurface::Connect)
        std::erase(remedies, Remedy::CloseWindow);
    else if (std::ranges::find(remedies, Remedy::CloseWindow) == remedies.end())
        remedies.push_back(Remedy::CloseWindow);
    return remedies;
}

std::string connectRemedySentence(const ConnectFact& fact)
{
    using Protocol::Sync::ByeReason;
    const auto refusalSentence = [&fact]() -> std::string {
        switch (fact.deniedReason) {
        case ByeReason::TokenMissing:
            return "This host requires an access token.";
        case ByeReason::TokenInvalid:
            return "This host doesn’t recognize the access token.";
        case ByeReason::TokenExpired:
            return "The access token for this host has expired.";
        case ByeReason::TokenRevoked:
            return "The access token for this host was revoked.";
        case ByeReason::OutsideScope:
            return "This access token doesn’t allow this request. Use a token with the required permissions.";
        case ByeReason::VersionMismatch: {
            // A platform no release publishes a host for has no commands to
            // copy, so the sentence says so rather than sending the person to
            // install one by hand.
            const std::string notice = decideUpdateAction(fact.update) == UpdateAction::ManualUpdate && isUnpublishedPlatform(fact)
                ? manualHostInstallCommands(fact.remotePlatform, fact.update.local.appVersion)
                : hostUpdateNotice(fact.update);
            return notice.empty() ? versionRefusalSentence(fact.update) : versionRefusalSentence(fact.update) + " " + notice;
        }
        case ByeReason::SessionGone:
            return fact.hasWorkspaceName ? "A new workspace opens under the same name, and Connect returns to it next time."
                                         : "A new workspace opens, and Connect returns to it next time.";
        case ByeReason::DuplicateHandshake:
        case ByeReason::NoConsent:
        case ByeReason::RemoteHostRefused:
        case ByeReason::HostFull:
        case ByeReason::Normal:
        case ByeReason::Malformed:
        case ByeReason::Oversize:
        case ByeReason::UnknownKind:
        case ByeReason::UnboundHandle:
        case ByeReason::TransferRefused:
        case ByeReason::Superseded:
        case ByeReason::ResumeInProgress:
        case ByeReason::Suspending:
        case ByeReason::HandingOver:
        case ByeReason::OutputBacklog:
        case ByeReason::Silent:
            break;
        }
        return std::string(Protocol::Sync::byeNotice(fact.deniedReason));
    };
    switch (fact.kind) {
    case ConnectFact::Kind::AttachRefused:
        return refusalSentence();
    case ConnectFact::Kind::ConsentDenied:
        return "The people on this workspace declined, or didn’t answer in time.";
    case ConnectFact::Kind::ConnectFailed:
        return connectFailureSentence(fact.connectFailure);
    case ConnectFact::Kind::ListingFailed:
        break;
    }
    // The host by the name the person gave it, inside a sentence; this
    // machine's own host by the machine.
    const std::string here = localDeviceObject(fact.update.localDevice);
    if (fact.update.isThisMachine) {
        switch (fact.listingFailure) {
        case ListingFailure::Unreachable:
        case ListingFailure::SignInRequired:
        case ListingFailure::AuthenticationAttemptsExhausted:
        case ListingFailure::EndedBeforeAnswer:
        case ListingFailure::NotInstalled:
            return "The Ubiquitty host on " + here + " didn’t answer. Try again.";
        case ListingFailure::InstanceGone:
            return "The Ubiquitty host you chose on " + here + " is no longer running. Try again to see the sessions " + here + " is running now.";
        case ListingFailure::Malformed:
            return "The Ubiquitty host on " + here + " sent a reply this app doesn’t understand. Try again.";
        case ListingFailure::InstanceUnreadable:
            return "This app cannot read the running host. Its sessions keep running. Use a matching build to open them.";
        case ListingFailure::None:
        case ListingFailure::Refused:
            break;
        }
    }
    const std::string host = fact.update.hostLabel.empty() ? std::string("the host") : fact.update.hostLabel;
    switch (fact.listingFailure) {
    case ListingFailure::None:
        return {};
    case ListingFailure::Unreachable:
        return "SSH couldn’t reach the host. Check its address and your network, then try again.";
    case ListingFailure::SignInRequired:
        return "SSH needs a password or an unlocked key, and it can’t ask for one here. Open an SSH shell, where SSH can ask.";
    case ListingFailure::AuthenticationAttemptsExhausted:
        return "The server closed the connection after too many authentication attempts. Set IdentityFile and IdentitiesOnly for this host in ~/.ssh/config so SSH offers only its key.";
    case ListingFailure::EndedBeforeAnswer:
        return "The host closed the SSH connection before Ubiquitty’s host answered. Open an SSH shell to see why ubiquitty-host didn’t start.";
    case ListingFailure::InstanceGone:
        return "The Ubiquitty host you chose on " + host + " is no longer running. Try again to see the sessions " + host + " is running now.";
    case ListingFailure::NotInstalled:
        // Only a push of a bundled host can serve a platform no release
        // publishes one for; without one, the sentence says that plainly
        // rather than sending the person to an install that does not exist.
        if (isUnpublishedPlatform(fact) && fact.deployAction != DeployAction::Push && fact.deployAction != DeployAction::ConfirmPush)
            return manualHostInstallCommands(fact.remotePlatform, fact.update.local.appVersion);
        switch (fact.deployAction) {
        case DeployAction::Push:
        case DeployAction::ConfirmPush:
            return "Ubiquitty isn’t installed on " + host + ". Ubiquitty can copy its host to ~/.ubiquitty there and start it. SSH still checks the host’s identity against your known_hosts file.";
        case DeployAction::ManualInstall:
            return localDeviceSubject(fact.update.localDevice) + " has no host build for that machine. Install ubiquitty-host on " + host + " by hand, then try again.";
        case DeployAction::Off:
            // The remedy beside it opens the options window by the name each
            // platform gives it, so the sentence leaves the window unnamed.
            return "Automatic install is turned off. Turn it on, or install ubiquitty-host on " + host + " by hand.";
        }
        return {};
    case ListingFailure::Refused:
        if (fact.deniedReason == ByeReason::Normal)
            return "The host refused to list its workspaces for this connection.";
        return refusalSentence();
    case ListingFailure::Malformed:
        return "The host sent a reply Ubiquitty doesn’t understand. Try again, and if this keeps happening, reinstall Ubiquitty on " + host + ".";
    case ListingFailure::InstanceUnreadable:
        return "This app cannot read the running host. Its sessions keep running. Use a matching build to open them.";
    }
    return {};
}

std::size_t connectRemedyCount(const ConnectFact& fact) { return connectRemedies(fact).size(); }

Remedy connectRemedyAt(const ConnectFact& fact, std::size_t index)
{
    const std::vector<Remedy> remedies = connectRemedies(fact);
    return index < remedies.size() ? remedies[index] : Remedy::CloseWindow;
}

} // namespace Ubiquitty::Session
