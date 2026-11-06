// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Session/ConnectRemedy.h — what a person can do about a connect
// that did not land, in the order a dialog offers it.
//
// A listing that failed, a handshake the host refused, a consent the host was
// denied, and a host with nothing installed each leave the person a short list
// of next actions: install, update, hand over a token, try again, open the
// shell ssh needs, start a new session. Which actions, and which one Return
// takes, is a decision — and a decision two frontends spelled separately would
// answer the same refusal with different buttons. So the list is decided here,
// once, and every surface renders it: the Connect window's states, the
// refusal sheet on a terminal window, and the Linux frontend.
//
// The first remedy is the default. A frontend titles each remedy and says the
// sentence beside them; it adds none and reorders none.
//
// Portable: no platform types. Exception/rtti-free (WebKit style).

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/DeployPolicy.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Transport/ConnectFailure.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

/// One action a person can take about a connect that did not land.
///
/// The values are explicit because the Swift mirrors pin them and the
/// accessibility identifiers name them.
enum class Remedy : std::uint8_t {
    /// Trust the host and push this app's bundled host, then connect.
    InstallAndConnect UBIQUITTY_SWIFT_NAME(installAndConnect) = 0,
    /// Copy the commands that install the host by hand.
    CopyInstallCommands UBIQUITTY_SWIFT_NAME(copyInstallCommands) = 1,
    /// Open the setting that turns automatic install on.
    OpenSettings UBIQUITTY_SWIFT_NAME(openSettings) = 2,
    /// Push this app's current host over an older one.
    UpdateHost UBIQUITTY_SWIFT_NAME(updateHost) = 3,
    /// Hand over an access token for the host.
    EnterToken UBIQUITTY_SWIFT_NAME(enterToken) = 4,
    /// Open the host's administration window.
    ManageHost UBIQUITTY_SWIFT_NAME(manageHost) = 5,
    /// Start a new session under the name the refused one carried.
    StartNewNamed UBIQUITTY_SWIFT_NAME(startNewNamed) = 6,
    /// Start a new unnamed session.
    StartNewSession UBIQUITTY_SWIFT_NAME(startNewSession) = 7,
    /// Choose another session the host holds.
    ChooseAnother UBIQUITTY_SWIFT_NAME(chooseAnother) = 8,
    /// Ask the host's people for consent again.
    AskAgain UBIQUITTY_SWIFT_NAME(askAgain) = 9,
    /// Repeat the same connect.
    TryAgain UBIQUITTY_SWIFT_NAME(tryAgain) = 10,
    /// Open an interactive ssh shell, where ssh can ask for a password or a
    /// passphrase.
    OpenSSHShell UBIQUITTY_SWIFT_NAME(openSSHShell) = 11,
    /// Correct the host's address.
    EditAddress UBIQUITTY_SWIFT_NAME(editAddress) = 12,
    /// Close the window the refused connect belonged to.
    CloseWindow UBIQUITTY_SWIFT_NAME(closeWindow) = 13,
};

/// Where the remedies are shown, which decides whether a window exists to
/// close.
enum class RemedySurface : std::uint8_t {
    /// The Connect window, before any terminal window exists.
    Connect UBIQUITTY_SWIFT_NAME(connect) = 0,
    /// A sheet on the terminal window whose connect was refused.
    TerminalWindow UBIQUITTY_SWIFT_NAME(terminalWindow) = 1,
};

/// What happened to one connect, as the facts the remedies are decided from.
struct ConnectFact {
    /// What kind of answer the connect met.
    enum class Kind : std::uint8_t {
        /// The listing the Connect window asked for failed.
        ListingFailed UBIQUITTY_SWIFT_NAME(listingFailed) = 0,
        /// The host refused the handshake.
        AttachRefused UBIQUITTY_SWIFT_NAME(attachRefused) = 1,
        /// The host's people declined the attach, or did not answer.
        ConsentDenied UBIQUITTY_SWIFT_NAME(consentDenied) = 2,
        /// The connection to the host never opened.
        ConnectFailed UBIQUITTY_SWIFT_NAME(connectFailed) = 3,
    };

    Kind kind { Kind::ListingFailed };
    /// Where the remedies are shown.
    RemedySurface surface { RemedySurface::Connect };
    /// Why the listing failed, for ``Kind::ListingFailed``.
    ListingFailure listingFailure { ListingFailure::Unreachable };
    /// Why the connection never opened, for ``Kind::ConnectFailed``.
    Transport::ConnectFailure connectFailure { Transport::ConnectFailure::Other };
    /// Why the host refused, for ``Kind::AttachRefused`` and for a listing
    /// that failed ``ListingFailure::Refused``.
    Protocol::Sync::ByeReason deniedReason { Protocol::Sync::ByeReason::TokenMissing };
    /// What an install would do, for ``ListingFailure::NotInstalled``, as
    /// ``decideDeployAction`` answered it.
    DeployAction deployAction { DeployAction::Off };
    /// What a version refusal says about the two builds.
    UpdateFacts update;
    /// The remote's `uname -sm`, for a host with nothing installed or one
    /// that refused for its version; empty when the host did not say.
    std::string remotePlatform;
    /// Whether the refused session carried a workspace name.
    bool hasWorkspaceName { false };
};

/// The remedies for one connect, the default first.
///
/// - Parameter fact: What happened.
/// - Returns: The remedies in the order a dialog offers them; the first is the
///   default. Never empty on a terminal window, which can always be closed;
///   empty in the Connect window only when nothing can be done.
[[nodiscard]] std::vector<Remedy> connectRemedies(const ConnectFact& fact);

/// The sentence a dialog shows beside the remedies, saying what happened in
/// terms of what the person can do.
///
/// - Parameter fact: What happened.
/// - Returns: The sentence, verbatim.
[[nodiscard]] std::string connectRemedySentence(const ConnectFact& fact);

/// The human-readable outcome of a typed host refusal, without wire jargon.
/// Unlike a remedy sentence, this does not imply a replacement was opened.
/// The exact protocol diagnostic remains available separately in the log.
[[nodiscard]] inline std::string_view connectRefusalNotice(Protocol::Sync::ByeReason reason)
{
    return Protocol::Sync::byeNotice(reason);
}

/// The number of remedies a list holds, for a caller that cannot iterate a
/// `std::vector` (Swift, C).
///
/// - Parameter fact: What happened.
/// - Returns: The count.
[[nodiscard]] std::size_t connectRemedyCount(const ConnectFact& fact);

/// One remedy by position, for a caller that cannot iterate a `std::vector`.
///
/// - Parameters:
///   - fact: What happened.
///   - index: The position, from 0.
/// - Returns: The remedy, or ``Remedy::CloseWindow`` past the end.
[[nodiscard]] Remedy connectRemedyAt(const ConnectFact& fact, std::size_t index);

} // namespace Ubiquitty::Session
