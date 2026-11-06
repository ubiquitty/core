// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Attachment selection and peer diagnostics are portable protocol values.
// A request names intent; only the authenticated host supplies authority and
// the exact serving identity in its answer.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace Ubiquitty::Protocol::Sync {

/// The most bytes an app version takes on the wire.
inline constexpr std::size_t maxAppVersionBytes = 32;
inline constexpr std::size_t maxCredentialBytes = 256;
inline constexpr std::size_t maxAgentLabelBytes = 256;
inline constexpr std::size_t maxHostPlatformBytes = 64;
inline constexpr std::size_t maxDaemonSocketNameBytes = 65'535;
static_assert(maxDaemonSocketNameBytes == std::numeric_limits<std::uint16_t>::max());
inline constexpr std::uint8_t knownServingBuildFlags = 0x03;

/// What one peer said about its build, never an authentication claim.
struct PeerBuild {
    bool isKnown = false;
    /// The wire version it speaks; nonzero when known.
    std::uint16_t wireVersion = 0;
    std::string appVersion;
};

/// Selects attachment or unbound host control without a magic session number.
enum class AdmissionTarget : std::uint8_t {
    Session = 0,
    Workspace = 1,
    Control = 2,
    Management = 3,
};

/// Whether a target admits host Calls without acquiring pane attachment state.
[[nodiscard]] constexpr bool isUnboundTarget(AdmissionTarget target)
{
    return target == AdmissionTarget::Control || target == AdmissionTarget::Management;
}

/// Names a session or workspace and the policy a fresh session is created with.
struct AdmissionRequest {
    AdmissionTarget target = AdmissionTarget::Session;
    std::uint32_t sessionID = 0;
    std::uint32_t workspaceID = 0;
    std::uint64_t expectedDaemonInstanceID = 0;
    std::string workspaceName {};
    std::string agentLabel {};
    std::string sshHost {};
    bool isAttachOnly = false;
    bool endsWhenDetached = false;
    bool servesClipboard = false;
};

/// The authenticated daemon's exact routing answer, not observation authority.
struct ServingSelection {
    AdmissionTarget target = AdmissionTarget::Session;
    std::uint32_t sessionID = 0;
    std::uint32_t workspaceID = 0;
    std::uint64_t daemonInstanceID = 0;
    std::string socketName;
    std::uint8_t buildFlags = 0;
    std::string hostPlatform {};
    /// The instance the client's saved reference named, when this one
    /// answers the reference with that instance's workspace: recreated from
    /// its record after the host restarted, or held live since that instance
    /// handed it over. Zero for any other admission. It is the fact that lets
    /// a client accept a daemon it did not expect as the one that holds its
    /// workspace now, under the `workspaceID` this one gives it.
    std::uint64_t restoredFromDaemonInstanceID = 0;
};

/// Whether a selection answers a request's saved reference with the
/// workspace as the instance that runs now holds it.
///
/// The request named a workspace on an instance that no longer serves it; the
/// answer names that instance, on the instance that runs now, and selects the
/// workspace of that reference: recreated from its record after a restart, or
/// held live since that instance handed it over. An instance that recreates a
/// workspace numbers it among its own, under the request's id only when that
/// id was free there, so the selection's workspace id need not repeat the
/// request's. A recreated workspace has fresh sessions, and one handed over
/// may have lost the session the request named, so the request's session id
/// is not carried.
///
/// - Parameters:
///   - request: What the client asked for.
///   - selection: What the host answered.
/// - Returns: Whether the answer is a restore of exactly that reference.
[[nodiscard]] constexpr bool isRestoredReferenceAnswer(const AdmissionRequest& request, const ServingSelection& selection)
{
    return selection.restoredFromDaemonInstanceID && request.target == AdmissionTarget::Workspace && selection.target == AdmissionTarget::Workspace
        && request.expectedDaemonInstanceID == selection.restoredFromDaemonInstanceID
        && selection.daemonInstanceID != selection.restoredFromDaemonInstanceID
        && request.workspaceID && selection.workspaceID;
}

/// Bounded diagnostics available even when a peer refuses before Welcome.
struct PeerMetadata {
    PeerBuild build;
    std::string platform;
};

} // namespace Ubiquitty::Protocol::Sync
