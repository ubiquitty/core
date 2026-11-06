// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Which binaries a host takes for its own: the successor it hands its
// sessions to, and the app and relay it treats as its person's.
//
// A host on macOS judges a binary by the code signature of what it runs. A
// release build trusts the release team's Developer ID signature and nothing
// else, so an unsigned or ad hoc binary is never its successor nor its app,
// wherever it lies. A development build, which runs unsigned or under a
// development certificate, also trusts the file at the path it was installed
// at and the files its app names. A platform that signs no code, Linux, trusts
// by the file alone
// (Docs/sync.md §6.13, Docs/capability-origin.md).
//
// The policy here is portable and pure; the platform reads the signatures
// (`CodeSignature.h`).

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Host {

/// The Developer ID team whose signature a host trusts as its release's.
inline constexpr std::string_view trustedSigningTeam = "5TC8J8TCS3";

/// The signing identifier of the host binary: codesign names a single file by
/// its file name, and the release signs it as such.
inline constexpr std::string_view hostSigningIdentifier = "ubiquitty-host";

/// The signing identifier of the released macOS app, its bundle identifier.
inline constexpr std::string_view appSigningIdentifier = "org.ubiquitty.Ubiquitty";

/// The static-code requirement the app checks before installing a successor
/// from an update archive. The host uses the same team and identifier below.
[[nodiscard]] inline std::string trustedHostSignatureRequirement()
{
    return "identifier \"" + std::string(hostSigningIdentifier)
        + "\" and anchor apple generic and certificate leaf[subject.OU] = \""
        + std::string(trustedSigningTeam) + "\"";
}

/// Who signed the code a process runs, as its signature says.
///
/// The values are explicit because the tests pin them.
enum class Signer : std::uint8_t {
    /// The platform signs no code.
    Unsupported = 0,
    /// No signature that holds for the code: none, one the code no longer
    /// matches, or one that could not be read.
    None = 1,
    /// An ad hoc signature, which names no signer.
    AdHoc = 2,
    /// A certificate other than the trusted team's: another team's, or one
    /// not issued by Apple, as a development certificate is.
    OtherSigner = 3,
    /// The trusted team's certificate, issued by Apple, with the code valid.
    Team = 4,
};

/// What a code signature says about the code a process runs.
struct CodeSignature {
    Signer signer = Signer::None;
    /// The signing identifier the signature names; empty without one.
    std::string identifier;

    friend bool operator==(const CodeSignature&, const CodeSignature&) = default;
};

/// How a host decides which binaries are its own.
enum class SigningPolicy : std::uint8_t {
    /// A release build: by the trusted team's signature alone.
    Release,
    /// A development build: by that signature, or by file.
    Development,
};

/// Whether a candidate successor is the file at the path its holder was
/// installed at.
enum class InstallPlace : std::uint8_t {
    AtInstallPath,
    Elsewhere,
};

/// How a holder admits a candidate as its successor.
enum class SuccessorRoute : std::uint8_t {
    /// It is not admitted.
    Refused,
    /// As the file now at the holder's install path, which the holder checks
    /// the candidate's running code against.
    ByPlace,
    /// As a host binary the trusted team signed, as the holder is, whose
    /// running code the signature check validated.
    ByTeam,
};

/// Whether the file a local peer runs is one a host trusts by identity: its
/// own executable, or an interface it resolved (`resolveTrustedLocalBinaries`).
enum class FileTrust : std::uint8_t {
    Trusted,
    Untrusted,
};

/// How a holder admits a candidate as its successor, if at all.
///
/// A host binary the trusted team signed succeeds a holder the team signed,
/// wherever it lies. A release build admits nothing else: a holder or a
/// candidate that is unsigned, ad hoc or another signer's keeps the sessions
/// where they are. A development build also admits the file at the holder's
/// install path, whatever signs it, and a platform that signs no code admits
/// that file alone.
///
/// - Parameters:
///   - policy: This build's.
///   - holder: The holder's own signature.
///   - candidate: The signature of the code the candidate runs.
///   - place: Whether the candidate is the file at the holder's install path.
/// - Returns: The route it is admitted by.
[[nodiscard]] inline SuccessorRoute successorRoute(SigningPolicy policy, const CodeSignature& holder, const CodeSignature& candidate, InstallPlace place)
{
    const bool isAtInstallPath = place == InstallPlace::AtInstallPath; // bool-ok: names the one fact it reports
    if (holder.signer == Signer::Unsupported || candidate.signer == Signer::Unsupported)
        return isAtInstallPath ? SuccessorRoute::ByPlace : SuccessorRoute::Refused;
    if (holder.signer == Signer::Team && candidate.signer == Signer::Team && candidate.identifier == hostSigningIdentifier)
        return SuccessorRoute::ByTeam;
    if (policy == SigningPolicy::Development && isAtInstallPath)
        return SuccessorRoute::ByPlace;
    return SuccessorRoute::Refused;
}

/// Whether a local peer runs one of the host's own binaries: its app, or a
/// relay of a host.
///
/// The trusted team's signature on the released app or on a host binary
/// makes it so; another binary the team signed, as `ubq`, is not. A release
/// build trusts nothing else. A development build also trusts the files the
/// host resolved by identity, and a platform that signs no code trusts those
/// alone.
///
/// - Parameters:
///   - policy: This build's.
///   - peer: The signature of the code the peer runs.
///   - fileTrust: Whether the file it runs is one the host resolved.
/// - Returns: Whether it is the host's own.
[[nodiscard]] inline bool isOwnBinary(SigningPolicy policy, const CodeSignature& peer, FileTrust fileTrust)
{
    const bool isTrustedFile = fileTrust == FileTrust::Trusted; // bool-ok: names the one fact it reports
    if (peer.signer == Signer::Unsupported)
        return isTrustedFile;
    if (peer.signer == Signer::Team && (peer.identifier == appSigningIdentifier || peer.identifier == hostSigningIdentifier))
        return true;
    return policy == SigningPolicy::Development && isTrustedFile;
}

} // namespace Ubiquitty::Host
