// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Decide what to do when a remote host signals (via RemoteBootstrap.h::deploySentinel)
// that it has no usable ubiquitty-host, or refuses this client for its version:
// push a bundled prebuilt, show manual-install guidance, or do nothing
// (auto-install off). Push-bundled-binary only — neither a deploy nor an update
// ever builds from source on the host. Pure scalars, no platform types.

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Sync/AppVersion.h"
#include "Core/Protocol/Sync/NativeAttachment.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

// The last `maxLines` lines of `text` after dropping trailing empty lines,
// rejoined with '\n' (distills a build log into a compact failure alert).
// maxLines == 0 yields an empty string.
inline std::string lastLines(std::string_view text, std::size_t maxLines)
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    std::size_t first = 0;
    if (lines.size() > maxLines)
        first = lines.size() - maxLines;
    std::string tail;
    for (std::size_t index = first; index < lines.size(); ++index) {
        if (index != first)
            tail.push_back('\n');
        tail += lines[index];
    }
    return tail;
}

// What the frontend should do for an auto-deploy request.
enum class DeployAction {
    // Auto-install is on, a bundled prebuilt exists, and this host is already a
    // trusted deploy target: stream it over the existing ssh channel, reconnect.
    Push,
    // Same as Push but this host has not been approved for deploy before: ask
    // the user (trust-on-first-use) before pushing, then remember the host.
    ConfirmPush,
    // Auto-install is on but no bundled prebuilt matches this platform: show the
    // user how to install ubiquitty-host themselves (no reconnect loop).
    ManualInstall,
    // Auto-install is turned off: show manual-install guidance, and say that
    // automatic install is disabled, so the user reads it as a setting rather
    // than a missing binary.
    Off,
};

// Decide the action for an auto-deploy request.
//   configAutoInstall  — the resolved `auto_install_host` setting.
//   platformArch       — the remote's `uname -sm`; carried for logging (presence
//                        already distilled into hasBundledBinary).
//   hasBundledBinary   — a readable bundled prebuilt host exists for this platform.
//   hostAlreadyTrusted — the user previously approved pushing to this ssh host.
// Off when auto-install is disabled (checked FIRST), else ManualInstall without a
// bundled binary, else Push (trusted) / ConfirmPush (first time).
inline DeployAction decideDeployAction(bool configAutoInstall, std::string_view platformArch, bool hasBundledBinary, bool hostAlreadyTrusted)
{
    (void)platformArch; // distilled into hasBundledBinary; kept for caller context
    if (!configAutoInstall)
        return DeployAction::Off;
    if (!hasBundledBinary)
        return DeployAction::ManualInstall;
    return hostAlreadyTrusted ? DeployAction::Push : DeployAction::ConfirmPush;
}

/// What kind of device this client runs on, as its frontend reports it.
///
/// The values are explicit because the Swift mirrors pin them.
enum class LocalDeviceKind : std::uint8_t {
    /// The frontend did not say.
    Unknown UBIQUITTY_SWIFT_NAME(unknown) = 0,
    Mac UBIQUITTY_SWIFT_NAME(mac) = 1,
    IPhone UBIQUITTY_SWIFT_NAME(iPhone) = 2,
    IPad UBIQUITTY_SWIFT_NAME(iPad) = 3,
    /// A Linux or other desktop machine.
    Computer UBIQUITTY_SWIFT_NAME(computer) = 4,
    /// A web page.
    Browser UBIQUITTY_SWIFT_NAME(browser) = 5,
};

/// The device this client runs on: a fact the frontend reports, which a
/// sentence names the way it names the remote host.
struct LocalDevice {
    LocalDeviceKind kind { LocalDeviceKind::Unknown };
    /// The name its owner gave it, e.g. "Sam’s MacBook Pro"; empty where the
    /// platform does not say.
    std::string name;
};

/// Names this device as a sentence opens with it.
///
/// - Parameter device: The device.
/// - Returns: Its name when it has one, else "This Mac", "This iPhone",
///   "This iPad", "This computer", "This browser" or "This device".
inline std::string localDeviceSubject(const LocalDevice& device)
{
    if (!device.name.empty())
        return device.name;
    switch (device.kind) {
    case LocalDeviceKind::Mac:
        return "This Mac";
    case LocalDeviceKind::IPhone:
        return "This iPhone";
    case LocalDeviceKind::IPad:
        return "This iPad";
    case LocalDeviceKind::Computer:
        return "This computer";
    case LocalDeviceKind::Browser:
        return "This browser";
    case LocalDeviceKind::Unknown:
        break;
    }
    return "This device";
}

/// Names this device inside a sentence.
///
/// - Parameter device: The device.
/// - Returns: Its name when it has one, else the subject's words in lower
///   case: "this iPhone".
inline std::string localDeviceObject(const LocalDevice& device)
{
    std::string words = localDeviceSubject(device);
    if (device.name.empty() && !words.empty())
        words[0] = 't';
    return words;
}

/// Names this device in a title: its kind, never its name, which a headline
/// would wrap.
///
/// - Parameter device: The device.
/// - Returns: "This Mac", "This iPhone" and so on, or "This Device".
inline std::string localDeviceTitle(const LocalDevice& device)
{
    const LocalDevice kindOnly { .kind = device.kind, .name = {} };
    const std::string words = localDeviceSubject(kindOnly);
    if (words == "This computer")
        return "This Computer";
    if (words == "This browser")
        return "This Browser";
    if (words == "This device")
        return "This Device";
    return words;
}

/// What a host refused for a version can be offered.
enum class UpdateAction : std::uint8_t {
    /// This client is older than the host, or this machine's own host has no
    /// bundled build to take it over.
    None,
    /// Push this app's bundled host over the install path (HostInstall.h).
    OfferHostUpdate,
    /// No bundled host serves the host's platform: show the commands that
    /// install a current one by hand.
    ManualUpdate,
    /// The host is this machine's own: install the host bundled with this app
    /// where this machine's host runs from, and start it, so the older host
    /// hands it every session (`Platform/POSIX/LocalHostInstall.h`). Nothing
    /// is uploaded and no session ends.
    TakeOverLocalHost,
};

/// The facts an update decision and its sentence read.
struct UpdateFacts {
    /// This client's build; this app's unless a test states another.
    Protocol::Sync::PeerBuild local { Protocol::Sync::localPeerBuild() };
    /// What the refusing host said about its build; not known when its
    /// refusal carried nothing this build reads.
    Protocol::Sync::PeerBuild host;
    /// The host's name as the person knows it, e.g. "Studio"; empty when the
    /// surface has none.
    std::string hostLabel;
    /// The device this client runs on, as the frontend reports it.
    LocalDevice localDevice;
    /// How many sessions the refusing host was holding when it last answered;
    /// 0 when it never did.
    std::size_t heldSessionCount { 0 };
    /// Whether a bundled host binary serves the host's platform. A caller
    /// that does not know the platform answers whether any bundled host
    /// exists: the update probes the platform before it uploads. For this
    /// machine's own host, whether this app bundles a host at all.
    bool hasBundledBinary { false };
    /// Whether the refusing host is this machine's own. Its build is the one
    /// this app bundles, and a sentence names the machine, never the host's
    /// label.
    bool isThisMachine { false };
};

/// Which end of a version refusal runs the older build.
///
/// - Parameter facts: The refusal's facts.
/// - Returns: The end whose build is the older, or
///   ``Protocol::Sync::OlderSide::Unknown`` when the host stated no build or
///   neither build orders before the other.
inline Protocol::Sync::OlderSide olderSideOf(const UpdateFacts& facts)
{
    return Protocol::Sync::olderSideOfBuilds(facts.local, facts.host);
}

/// Decides what a version refusal offers.
///
/// A known newer host is never replaced. Builds with equal version numbers
/// can still be incompatible; the user can install this app's matching host.
/// Installation checks for running sessions before replacing anything.
///
/// - Parameter facts: The refusal's facts.
/// - Returns: The action.
inline UpdateAction decideUpdateAction(const UpdateFacts& facts)
{
    if (olderSideOf(facts) == Protocol::Sync::OlderSide::Client)
        return UpdateAction::None;
    // The host bundled with this app is this machine's build: there is
    // nothing to upload and nothing to install by hand.
    if (facts.isThisMachine)
        return facts.hasBundledBinary ? UpdateAction::TakeOverLocalHost : UpdateAction::None;
    return facts.hasBundledBinary ? UpdateAction::OfferHostUpdate : UpdateAction::ManualUpdate;
}

/// The sentence that says what an update does, before the person asks for it.
///
/// Updating asks for live handover. A holder that refuses keeps its sessions.
///
/// - Parameter facts: The refusal's facts.
/// - Returns: The sentence, verbatim for the dialog; empty when no update is
///   offered.
inline std::string hostUpdateNotice(const UpdateFacts& facts)
{
    switch (decideUpdateAction(facts)) {
    case UpdateAction::None:
        return {};
    case UpdateAction::ManualUpdate:
        return localDeviceSubject(facts.localDevice) + " has no host build for that machine. Install a current ubiquitty-host there by hand, then connect again.";
    case UpdateAction::TakeOverLocalHost:
        return "Updating moves its sessions to the host included with this app without ending them.";
    case UpdateAction::OfferHostUpdate:
        break;
    }
    const std::string lead = "Ubiquitty copies its current host there and connects to it.";
    if (facts.heldSessionCount == 0)
        return lead + " The host holds no sessions to keep.";
    if (facts.heldSessionCount == 1)
        return lead + " Its running session moves to the new host. If handover is refused, the old host keeps it running.";
    return lead + " Its " + std::to_string(facts.heldSessionCount) + " running sessions move to the new host. If handover is refused, the old host keeps them running.";
}

/// Names one build's version in parentheses, as a sentence follows a name
/// with it.
///
/// - Parameter build: The build.
/// - Returns: " (0.2.0.7)", or nothing when it sent no app version.
inline std::string buildFacts(const Protocol::Sync::PeerBuild& build)
{
    return build.appVersion.empty() ? std::string() : " (" + build.appVersion + ")";
}

/// The headline of a version refusal.
///
/// - Parameter facts: The refusal's facts.
/// - Returns: The title, in title case, naming the older end.
inline std::string versionRefusalTitle(const UpdateFacts& facts)
{
    if (facts.isThisMachine) {
        const std::string here = localDeviceTitle(facts.localDevice);
        if (olderSideOf(facts) == Protocol::Sync::OlderSide::Client)
            return here + " Runs a Newer Ubiquitty Host";
        if (facts.host.isKnown && olderSideOf(facts) == Protocol::Sync::OlderSide::Unknown)
            return here + " Runs a Different Build of the Ubiquitty Host";
        return here + " Runs an Older Ubiquitty Host";
    }
    const std::string host = facts.hostLabel.empty() ? "The Host" : facts.hostLabel;
    if (olderSideOf(facts) == Protocol::Sync::OlderSide::Client)
        return localDeviceTitle(facts.localDevice) + " Runs an Older Ubiquitty";
    if (facts.host.isKnown && olderSideOf(facts) == Protocol::Sync::OlderSide::Unknown)
        return host + " Runs a Different Build of Ubiquitty";
    return host + " Runs an Older Ubiquitty Host";
}

/// The sentence a version refusal says: that this device and the host run
/// different builds, which is older when their versions order, and which end
/// to update. It names versions a person can compare, never the wire.
///
/// - Parameter facts: The refusal's facts.
/// - Returns: The sentence, verbatim.
inline std::string versionRefusalSentence(const UpdateFacts& facts)
{
    if (facts.isThisMachine) {
        // One machine at both ends: the app and the host it runs.
        const std::string hereObject = localDeviceObject(facts.localDevice);
        const std::string host = "The Ubiquitty host on " + hereObject;
        const std::string hostObject = "the Ubiquitty host on " + hereObject;
        if (!facts.host.isKnown)
            return host + " doesn’t say which build it runs, so it’s older than this app" + buildFacts(facts.local) + ".";
        switch (olderSideOf(facts)) {
        case Protocol::Sync::OlderSide::Host:
            return host + " is from an older build" + buildFacts(facts.host) + " than this app" + buildFacts(facts.local) + ".";
        case Protocol::Sync::OlderSide::Client:
            return "This app is from an older build of Ubiquitty" + buildFacts(facts.local) + " than " + hostObject + buildFacts(facts.host)
                + ". Update Ubiquitty on " + hereObject + ".";
        case Protocol::Sync::OlderSide::Unknown:
            break;
        }
        if (!facts.local.appVersion.empty() && facts.local.appVersion == facts.host.appVersion)
            return "This app and " + hostObject + " run different builds of Ubiquitty " + facts.local.appVersion + ".";
        return "This app" + buildFacts(facts.local) + " and " + hostObject + buildFacts(facts.host) + " are from different builds of Ubiquitty.";
    }
    const bool hasLabel = !facts.hostLabel.empty();
    const std::string hostHost = hasLabel ? facts.hostLabel + "’s Ubiquitty Host" : "The Ubiquitty Host there";
    const std::string hostObject = hasLabel ? facts.hostLabel + "’s host" : "the host";
    const std::string place = hasLabel ? facts.hostLabel : "that machine";
    const std::string here = localDeviceSubject(facts.localDevice);
    const std::string hereObject = localDeviceObject(facts.localDevice);
    if (!facts.host.isKnown)
        return hostHost + " doesn’t say which build it runs, so it’s older than " + hereObject + buildFacts(facts.local) + ". Update the host on " + place + ".";
    switch (olderSideOf(facts)) {
    case Protocol::Sync::OlderSide::Host:
        return hostHost + " is from an older build" + buildFacts(facts.host) + " than " + hereObject + buildFacts(facts.local) + ". Update the host on " + place + ".";
    case Protocol::Sync::OlderSide::Client:
        return here + " is from an older build of Ubiquitty" + buildFacts(facts.local) + " than " + hostObject + buildFacts(facts.host) + ". Update Ubiquitty on " + hereObject + ".";
    case Protocol::Sync::OlderSide::Unknown:
        break;
    }
    if (!facts.local.appVersion.empty() && facts.local.appVersion == facts.host.appVersion)
        return here + " and " + hostObject + " run different builds of Ubiquitty " + facts.local.appVersion + ". Install the same build on both.";
    return here + buildFacts(facts.local) + " and " + hostObject + buildFacts(facts.host) + " are from different builds of Ubiquitty. Install the same build on both.";
}

/// What updating this machine's host left undone, as the facts its words
/// are chosen from.
struct LocalHostUpgradeFacts {
    /// Hosts of another build, none of them newer, still serving on this
    /// machine afterwards.
    std::size_t olderHostCount { 0 };
    /// Whether moving the app would let them hand their sessions over: one
    /// of them refuses this app's host because the app is on another volume
    /// than the control directory, where its host could only be copied.
    bool isMoveAdvised { false };
    /// Whether a host from a newer build is installed, which this app leaves
    /// as it is.
    bool isNewerInstalled { false };
};

/// The headline of an update of this machine's host that left something
/// undone.
///
/// - Parameter facts: What the update found.
/// - Returns: The title, in title case; empty when every session moved.
inline std::string localHostUpgradeTitle(const LocalHostUpgradeFacts& facts)
{
    if (facts.isNewerInstalled)
        return "This Mac Runs a Newer Ubiquitty Host";
    if (!facts.olderHostCount)
        return {};
    return facts.olderHostCount == 1 ? "An Older Ubiquitty Host Keeps Its Sessions" : "Older Ubiquitty Hosts Keep Their Sessions";
}

/// What an update of this machine's host that left something undone says,
/// and what the person can do about it.
///
/// - Parameter facts: What the update found.
/// - Returns: The sentences, verbatim; empty when every session moved.
inline std::string localHostUpgradeSentence(const LocalHostUpgradeFacts& facts)
{
    if (facts.isNewerInstalled)
        return "The Ubiquitty host on this Mac is from a newer build than this app, which leaves it as it is. Update Ubiquitty to open its sessions.";
    if (!facts.olderHostCount)
        return {};
    std::string sentence = facts.olderHostCount == 1
        ? "An older Ubiquitty host on this Mac didn’t hand its sessions to the host included with this app. It keeps them until they end."
        : "Older Ubiquitty hosts on this Mac didn’t hand their sessions to the host included with this app. They keep them until they end.";
    if (facts.isMoveAdvised)
        sentence += " To bring them to this app’s host, move Ubiquitty to the Applications folder, then open it again.";
    return sentence;
}

/// The version stamp a serialised deploy-trust list opens with.
///
/// A list written by a build that spelled it differently is refused on load
/// rather than read as this layout; a refused list trusts nobody, so a store
/// whose meaning changed costs a second consent and never a silent push.
inline constexpr std::uint8_t deployTrustRecordVersion = 1;

/// The most hosts a deploy-trust list holds; a longer one is not a list this
/// build wrote and is refused whole.
inline constexpr std::size_t maxDeployTrustedHosts = 1024;

/// Writes the hosts trusted to receive a push as the bytes a store keeps.
///
/// - Parameter hosts: The trusted ssh destinations, in the order trusted.
/// - Returns: The bytes: a version byte, then each host as a big-endian
///   16-bit length and its bytes.
inline std::string encodeDeployTrust(const std::vector<std::string>& hosts)
{
    std::string bytes;
    bytes.push_back(static_cast<char>(deployTrustRecordVersion));
    for (const std::string& host : hosts) {
        if (host.empty() || host.size() > 0xFFFF)
            continue;
        bytes.push_back(static_cast<char>((host.size() >> 8) & 0xFF));
        bytes.push_back(static_cast<char>(host.size() & 0xFF));
        bytes += host;
    }
    return bytes;
}

/// Reads bytes a store kept as the hosts trusted to receive a push.
///
/// - Parameter bytes: What the store answered.
/// - Returns: The hosts, or nothing when the bytes carry another version, cut
///   a host short, or name more hosts than a list holds.
inline std::optional<std::vector<std::string>> decodeDeployTrust(std::string_view bytes)
{
    if (bytes.empty() || static_cast<std::uint8_t>(bytes.front()) != deployTrustRecordVersion)
        return std::nullopt;
    std::vector<std::string> hosts;
    std::size_t offset = 1;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 2 || hosts.size() >= maxDeployTrustedHosts)
            return std::nullopt;
        const std::size_t length = (static_cast<std::size_t>(static_cast<std::uint8_t>(bytes[offset])) << 8)
            | static_cast<std::uint8_t>(bytes[offset + 1]);
        offset += 2;
        if (length == 0 || bytes.size() - offset < length)
            return std::nullopt;
        hosts.emplace_back(bytes.substr(offset, length));
        offset += length;
    }
    return hosts;
}

// How many lines of a failed push's output a person is shown: enough for ssh's
// own complaint and the remote shell's, short enough to fit an alert.
inline constexpr std::size_t deployFailureTailLines = 20;

/// Reduces a failed push's output to what a person is shown.
///
/// The tail rather than the head, because the last lines are ssh's or the
/// remote shell's own words for what went wrong; a push that printed nothing
/// still says something, since an alert with an empty body reads as a bug in
/// the terminal rather than a refusal on the host.
///
/// - Parameter output: Everything the push printed, standard error included.
/// - Returns: The tail, or a fixed sentence for a push that printed nothing.
inline std::string deployFailureDetail(std::string_view output)
{
    std::string tail = lastLines(output, deployFailureTailLines);
    return tail.empty() ? std::string("the host install did not complete") : tail;
}

/// Names the published host a remote platform downloads.
///
/// Keyed like the bundled deploy map, so the platform a push would have served
/// is the platform a manual install is told about. The name is the file's in a
/// release's published host directory.
///
/// - Parameter platformArch: The remote's `uname -sm`.
/// - Returns: The file name, empty for a platform no release publishes a host
///   for.
inline std::string_view publishedHostAsset(std::string_view platformArch)
{
    if (platformArch == "Linux x86_64")
        return "ubiquitty-host-linux-x86_64";
    if (platformArch == "Linux aarch64")
        return "ubiquitty-host-linux-arm64";
    if (platformArch == "Darwin arm64")
        return "ubiquitty-host-macos-arm64";
    return {};
}

/// Builds the commands a person runs on the host to install `ubiquitty-host`
/// by hand.
///
/// The download is the published host of `appVersion`'s named version, never
/// the latest one, so the host matches the client that offered the commands;
/// a build between named versions takes the one it follows. Before anything
/// moves, the file's SHA-256 is compared with the release's `SHA256SUMS`
/// line. The digests are compared as text rather than through
/// `sha256sum -c`, because macOS's `sha256sum` passes an empty list. The
/// download lands beside the host and is renamed over it, so a host that is
/// running keeps the file it started from and a refused download is never
/// executable at the host's path.
///
/// The commands run under `sh -c` with `set -eu`, so a failed step stops the
/// rest in whichever shell they are pasted into; for that reason they hold no
/// single quote of their own.
///
/// - Parameters:
///   - platformArch: The remote's `uname -sm`.
///   - appVersion: The version of the client the host must match.
/// - Returns: The commands, newline-separated, ready to paste. A platform no
///   release publishes a host for, or a version that names no release, gets a
///   sentence that says so instead, and nothing to run.
inline std::string manualHostInstallCommands(std::string_view platformArch, std::string_view appVersion)
{
    const std::string_view asset = publishedHostAsset(platformArch);
    if (asset.empty())
        return "There’s no prebuilt ubiquitty-host for " + (platformArch.empty() ? std::string("this platform") : std::string(platformArch)) + " yet.";
    const std::optional<Protocol::Sync::AppVersion> version = Protocol::Sync::parseAppVersion(appVersion);
    if (!version)
        return "This build of Ubiquitty isn’t a release, so no prebuilt ubiquitty-host matches it.";
    const std::string named = std::to_string(version->major) + "." + std::to_string(version->minor) + "." + std::to_string(version->patch);
    return "sh -c 'set -eu\n"
           "url=https://ubiquitty.org/download/host/"
        + named + "\nasset=" + std::string(asset)
        + "\n"
          "mkdir -p ~/.ubiquitty\n"
          "cd ~/.ubiquitty\n"
          "curl -fsSL -o ubiquitty-host.download \"$url/$asset\"\n"
          "want=$(curl -fsSL \"$url/SHA256SUMS\" | grep \"  $asset\\$\" || true)\n"
          "if command -v sha256sum >/dev/null; then got=$(sha256sum ubiquitty-host.download); else got=$(shasum -a 256 ubiquitty-host.download); fi\n"
          "if [ \"${got%% *}\" != \"${want%% *}\" ]; then echo \"The download doesn’t match SHA256SUMS.\" >&2; exit 1; fi\n"
          "chmod 755 ubiquitty-host.download\n"
          "mv -f ubiquitty-host.download ubiquitty-host\n"
          "./ubiquitty-host --version'";
}

} // namespace Ubiquitty::Session
