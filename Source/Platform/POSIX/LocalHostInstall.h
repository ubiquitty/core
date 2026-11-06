// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/SignaturePolicy.h"

// Bringing this machine's host up to the build an app bundles, by handover.
//
// A running host hands its sessions only to a successor whose executable is
// the file at the path the holder was installed at (Docs/sync.md §6.13). A
// host is installed at one of two paths: the app bundle's helper, where an
// app starts it, and the control directory's `ubiquitty-host`, where an SSH
// bootstrap installs and starts it. The app keeps starting its bundled
// helper, whose host trusts the app beside it, and makes the control
// directory's install that very file, so a holder installed at either path
// admits the successor the app starts. A host started from the install trusts
// the same app through the name the install leaves beside it
// (`Host::interfacePathForHost`).
//
// Native file and socket work stays behind this seam; Core supplies the paths
// and the daemon start, and words what is left undone from the facts here
// (`Session::LocalHostUpgradeFacts`).

#include "Base/SwiftInterop.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// How making the installed host the bundled build ended.
///
/// The values are explicit because the Swift mirrors pin them.
enum class LocalHostInstallKind : std::uint8_t {
    /// Nothing was asked: a development build installs into, and starts a
    /// host in, only a control directory `UBIQUITTY_HOME` names.
    NotAttempted UBIQUITTY_SWIFT_NAME(notAttempted) = 0,
    /// The installed host is the bundled file itself already.
    Current UBIQUITTY_SWIFT_NAME(current) = 1,
    /// The installed host is now the bundled file itself, by a hard link.
    Linked UBIQUITTY_SWIFT_NAME(linked) = 2,
    /// The installed host holds the bundled host's bytes in a file of its
    /// own, because the bundled file could not be linked there. A holder
    /// installed at the control directory's path does not admit the bundled
    /// host as its successor.
    Copied UBIQUITTY_SWIFT_NAME(copied) = 3,
    /// A host from a newer build is installed; it is left as it is, and
    /// nothing is started over it.
    NewerInstalled UBIQUITTY_SWIFT_NAME(newerInstalled) = 4,
    /// Nothing changed: the install could not be written, or the installed
    /// host did not say its version in time.
    Refused UBIQUITTY_SWIFT_NAME(refused) = 5,
};

/// What making the installed host the bundled build did.
struct LocalHostInstallOutcome {
    LocalHostInstallKind kind { LocalHostInstallKind::NotAttempted };
    /// For ``LocalHostInstallKind/refused``, why nothing changed; for
    /// ``LocalHostInstallKind/copied``, why the bundled file was not linked;
    /// for ``LocalHostInstallKind/newerInstalled``, the installed version.
    /// In words for a log.
    std::string reason;
    /// Whether the bundled host is on another volume than the install, which
    /// only a copy reaches.
    bool isOnAnotherVolume = false; // bool-ok: names the one fact it reports
};

/// Makes the file at `installPath` the bundled host's.
///
/// A hard link when the bundled file is this user's, writable by no one
/// else, and on the same filesystem; else an owner-only copy. Either is
/// staged beside the install under a name of its own and renamed over it,
/// so a host running the file it replaces keeps the image it runs, and
/// nothing reads a half-written host. An installed host from a newer build
/// is never replaced, nor one that does not say its version in time: a slow
/// host is not known to be older. One that cannot say a version is. Nothing
/// is written in a directory that is not a real directory owned by this
/// user, nor over anything at `installPath` that is not a regular file.
/// Beside the install, `<installPath>.interface` names the bundled app's
/// executable.
///
/// - Parameters:
///   - bundledHostPath: The host bundled with the app, absolute.
///   - installPath: Where the host is installed, absolute: the control
///     directory's `ubiquitty-host` (``localHostInstallPath``).
///   - bundledVersion: The bundled host's app version, which a newer
///     installed host's outranks.
/// - Returns: What was done, with the reason when it fell short.
[[nodiscard]] LocalHostInstallOutcome installBundledHost(const std::string& bundledHostPath, const std::string& installPath, std::string_view bundledVersion);

/// Whether the app is a build to run on its person's own host.
///
/// The values are explicit because the Swift mirrors pin them.
enum class LocalHostBuild : std::uint8_t {
    /// An installed app: the host it bundles is the one this machine runs.
    Release UBIQUITTY_SWIFT_NAME(release) = 0,
    /// An app built in a checkout. It runs beside the person's own host, so
    /// it upgrades only a control directory `UBIQUITTY_HOME` names.
    Development UBIQUITTY_SWIFT_NAME(development) = 1,
};

/// Whether an app of `build` upgrades this machine's host at all.
///
/// - Parameter build: Whether the app is installed or built in a checkout.
/// - Returns: Whether ``upgradeLocalHost`` would do anything.
[[nodiscard]] bool isLocalHostUpgradable(LocalHostBuild build);

/// Whether to report remaining hosts or attempt to take them over as well.
enum class LocalHostTakeoverMode : std::uint8_t {
    Inspect,
    Start,
};

/// The result of asking an installed host to succeed older local daemons.
struct LocalHostTakeoverOutcome {
    bool didStartSuccessor = false; // bool-ok: names the one fact it reports
    bool isHostAnswering = false; // bool-ok: names the one fact it reports
    std::size_t olderHostCount = 0;
    std::size_t refusingHostCount = 0;
};

/// Asks older daemons to hand over to `hostPath`, without installing a file.
/// Inspect only counts them. Start may restart an idle successor to retry a
/// handover; a daemon holding sessions is never stopped. Refusing holders
/// keep serving, and the successor's log records their reasons.
///
/// - Parameters:
///   - hostPath: The installed successor's absolute executable path.
///   - mode: Whether a successor may be started.
/// - Returns: Whether the successor answers and how many older hosts remain.
[[nodiscard]] LocalHostTakeoverOutcome takeOverLocalHosts(const std::string& hostPath, LocalHostTakeoverMode mode);

/// What bringing this machine's host up to the bundled build did.
struct LocalHostUpgradeOutcome {
    LocalHostInstallOutcome install;
    /// Whether the bundled host was started, or started again, to take the
    /// sessions of a host of an older build over.
    bool didStartSuccessor = false; // bool-ok: names the one fact it reports
    /// Whether the bundled host answers on its socket afterwards.
    bool isBundledHostAnswering = false; // bool-ok: names the one fact it reports
    /// Whether the installed host is the bundled file itself, which a host
    /// installed there admits as its successor.
    bool isInstalled = false; // bool-ok: names the one fact it reports
    /// Hosts of another build, none of them newer, still serving on this
    /// machine afterwards.
    std::size_t olderHostCount = 0;
    /// Of those, the ones whose own executable path names another file than
    /// the bundled host's, which refuse it as their successor.
    std::size_t refusingHostCount = 0;
};

/// Runs the install and takeover for a verified host from an update archive.
/// Its version is the archive's checked CFBundleShortVersionString, not the old app's.
[[nodiscard]] LocalHostUpgradeOutcome upgradeLocalHostForUpdate(const std::string& hostPath, LocalHostBuild build, const std::string& version);

/// Installs the bundled host, then has it take over every host of an older
/// build running on this machine.
///
/// Each host is asked who it is, through the bundled host's relay: its
/// answer names its build, and the instance of one that trusts the relay,
/// so a socket a successor inherited counts as the successor's. The bundled
/// host is started when a host of another build, not a newer one, answers
/// and it does not. One that runs and holds nothing is asked to stop and is
/// started again, so it asks the older hosts once more as it starts; one
/// that runs a session takes no other host's, and is not asked. A host that
/// refuses keeps its sessions and serves beside the bundled one, the bundled
/// host's log says why, and the outcome counts it. Nothing is started over a
/// newer installed host. Callers serialize upgrades.
///
/// - Parameters:
///   - bundledHostPath: The host bundled with the app, absolute.
///   - build: Whether the app is installed or built in a checkout.
/// - Returns: What was installed and started, and what is left undone.
[[nodiscard]] LocalHostUpgradeOutcome upgradeLocalHost(const std::string& bundledHostPath, LocalHostBuild build);

} // namespace Ubiquitty::Session
