// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// How a bundled `ubiquitty-host` reaches a remote host and replaces the one
// there: the probe that asks the host what it is and how many sessions its
// running daemon holds, the install that verifies an upload before anything
// is replaced, and the reading of what each printed.
//
// Transport::remoteShellCommand encodes each script as one SSH exec request
// that explicitly runs /bin/sh, independently of the remote login shell.
// Nothing here builds from source: a platform no bundled binary serves is told how to
// install by hand (DeployPolicy.h), never compiled for.
//
// Sessions are counted rather than asked for. The daemon an update replaces
// is by definition a build this client cannot speak to, so the count comes
// from the process table: each live session is one shell the daemon forked,
// and the daemon's pid file names it under the socket leaf its binary hashes
// to (DaemonSocketLeaf.h).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// The line the probe prints: the live-session count, then `uname -sm`.
inline constexpr std::string_view hostProbePrefix = "ubiquitty-host-probe ";

/// The line an install prints when the new host is in place.
inline constexpr std::string_view hostInstalledPrefix = "ubiquitty-deploy-ok";

/// The line an install prints when the new host waits beside the running one.
inline constexpr std::string_view hostDeferredPrefix = "ubiquitty-deploy-deferred ";

/// The installed successor could not take every predecessor's sessions.
inline constexpr std::string_view hostHandoverPendingPrefix = "ubiquitty-deploy-handover-pending ";

/// `ubiquitty-host --take-over` leaves a predecessor serving its sessions.
inline constexpr int hostHandoverPendingStatus = 75;

/// The line an install prints when it refused, followed by why.
inline constexpr std::string_view hostInstallFailedPrefix = "ubiquitty-deploy-failed: ";

/// The longest `uname -sm` a probe answer is read with; anything longer is
/// not a platform any bundled binary is keyed by.
inline constexpr std::size_t maxProbedPlatformBytes = 64;

/// The shell fragment that counts the sessions the daemon of `$B` holds.
///
/// Reads `$B` (the installed binary) and `$C` (the control directory), and
/// sets `$L` to the count and `$P` to the daemon's pid, empty when no daemon
/// of that binary is running. The count is the daemon's direct children: a
/// session is a shell the daemon forked, so a daemon with none holds none.
///
/// - Returns: The fragment, ending in `; `.
inline std::string liveSessionCountFragment()
{
    return "L=0; P=\"\"; H=\"\"; "
           "if [ -f \"$B\" ]; then H=$( { shasum -a 256 \"$B\" || sha256sum \"$B\"; } 2>/dev/null | cut -c1-16); fi; "
           "if [ -n \"$H\" ] && [ -f \"$C/host-$H.pid\" ]; then P=$(cat \"$C/host-$H.pid\" 2>/dev/null); fi; "
           "case \"$P\" in \"\"|*[!0-9]*) P=\"\" ;; esac; "
           "if [ -n \"$P\" ] && kill -0 \"$P\" 2>/dev/null; then L=$(ps -A -o ppid= | grep -c \"^ *$P\\$\"); else P=\"\"; fi; ";
}

/// The shell fragment that names the paths every script here shares.
///
/// `$D` is the install directory the bootstrap runs the host from, `$B` the
/// host in it, and `$C` the control directory the daemon keeps its pid files
/// in, which `UBIQUITTY_HOME` moves exactly as it moves the daemon's own.
///
/// - Returns: The fragment, ending in `; `.
inline std::string hostInstallPathsFragment()
{
    return "D=\"$HOME/.ubiquitty\"; [ ! -L \"$D\" ] || exit 1; "
           "mkdir -p \"$D\" && chmod 700 \"$D\" || exit 1; [ -d \"$D\" ] || exit 1; "
           "B=\"$D/ubiquitty-host\"; C=\"${UBIQUITTY_HOME:-$D}\"; ";
}

/// The shell fragment that replaces `$B` with `$1`'s file, keeping the host
/// it replaces as the one dated backup.
///
/// Only the newest backup is kept: one is what a person rolls back to, and an
/// update a week would otherwise fill the directory with every host ever run.
///
/// - Parameter source: The file moved into place, a shell word.
/// - Returns: The fragment, ending in `; `.
inline std::string promoteHostFragment(std::string_view source)
{
    return "rm -f \"$D\"/ubiquitty-host.backup-*; "
           "if [ -f \"$B\" ]; then cp -p \"$B\" \"$D/ubiquitty-host.backup-$(date +%Y%m%d%H%M%S)\" || fail \"the current host could not be kept as a backup\"; fi; "
           "mv -f "
        + std::string(source) + " \"$B\" || fail \"the new host could not be moved into place\"; ";
}

/// The command that asks a host how many sessions its daemon holds and what
/// platform it is.
///
/// Prints one ``hostProbePrefix`` line. It changes nothing on the host, so it
/// is safe to run before any consent.
///
/// - Returns: The script, for one ssh argument.
inline std::string hostProbeCommand()
{
    return "D=\"$HOME/.ubiquitty\"; B=\"$D/ubiquitty-host\"; C=\"${UBIQUITTY_HOME:-$D}\"; " + liveSessionCountFragment()
        + "printf \"\\n" + std::string(hostProbePrefix) + "%s %s\\n\" \"$L\" \"$(uname -sm)\"";
}

/// What a probe found.
struct HostProbe {
    /// How many sessions the running daemon holds; 0 when none runs.
    std::size_t liveSessionCount { 0 };
    /// The host's `uname -sm`.
    std::string platform;
};

/// Reads a probe's output.
///
/// The last probe line wins: a login banner printed before it is ignored, and
/// so is anything that only looks like one.
///
/// - Parameter output: Everything the probe printed.
/// - Returns: The answer, or nothing when no well-formed line arrived.
inline std::optional<HostProbe> parseHostProbe(std::string_view output)
{
    const std::size_t at = output.rfind(hostProbePrefix);
    if (at == std::string_view::npos || (at != 0 && output[at - 1] != '\n'))
        return std::nullopt;
    std::string_view line = output.substr(at + hostProbePrefix.size());
    line = line.substr(0, line.find('\n'));
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    HostProbe probe;
    std::size_t digits = 0;
    while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
        // Eight digits are more sessions than a process table holds; more is
        // not a count.
        if (digits == 8)
            return std::nullopt;
        probe.liveSessionCount = probe.liveSessionCount * 10 + static_cast<std::size_t>(line[digits] - '0');
        ++digits;
    }
    if (digits == 0 || digits >= line.size() || line[digits] != ' ')
        return std::nullopt;
    const std::string_view platform = line.substr(digits + 1);
    if (platform.empty() || platform.size() > maxProbedPlatformBytes)
        return std::nullopt;
    probe.platform = std::string(platform);
    return probe;
}

/// When an install may replace the running host.
enum class HostInstallMode : std::uint8_t {
    /// Replace it only while its daemon holds no session; refuse otherwise.
    /// What an update runs after a probe found none, so a session started
    /// between the probe and the install is not ended by it.
    IfIdle,
    /// Replace it now and ask its daemon to hand its live sessions over.
    /// A refused handover leaves the old daemon serving them.
    Now,
    /// Verify the upload and keep it beside the running host; the bootstrap
    /// moves it into place on the first connect after the daemon holds no
    /// session (see ``remoteBootstrapCommand``).
    WhenSessionsEnd,
};

/// Whether `text` is a lowercase SHA-256 in hex.
///
/// - Parameter text: The candidate.
/// - Returns: Whether it is 64 lowercase hex digits.
[[nodiscard]] constexpr bool isSHA256Hex(std::string_view text)
{
    if (text.size() != 64)
        return false;
    for (const char character : text) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
            return false;
    }
    return true;
}

/// The command that installs the host binary streamed on its standard input.
///
/// The upload lands as `ubiquitty-host.new` and nothing else moves until it
/// has passed every check: its SHA-256 is the one this client computed, and it
/// runs on this host (`--version`, then the `--help` the bootstrap gates every
/// connect on). Only then is the running host kept as a dated backup and the
/// new one renamed over it — a rename, so a daemon executing the old file
/// keeps the image it is running. Validation failures remove the upload and
/// leave the install unchanged. After promotion, the successor attempts live
/// handover; refusing predecessors keep their sessions and are reported as
/// pending. A startup failure leaves the new executable installed too.
///
/// - Parameters:
///   - sha256: The upload's SHA-256, lowercase hex.
///   - mode: When the running host may be replaced.
/// - Returns: The script for one ssh argument, or empty for a digest that is
///   not one.
inline std::string hostInstallCommand(std::string_view sha256, HostInstallMode mode)
{
    if (!isSHA256Hex(sha256))
        return {};
    const std::string digest(sha256);
    std::string script = hostInstallPathsFragment();
    script += "N=\"$D/ubiquitty-host.new\"; ";
    script += "fail() { rm -f \"$N\"; printf \"\\n" + std::string(hostInstallFailedPrefix) + "%s\\n\" \"$1\"; exit 1; }; ";
    script += "cat > \"$N\" || fail \"the upload could not be written to $N\"; ";
    script += "S=$( { shasum -a 256 \"$N\" || sha256sum \"$N\"; } 2>/dev/null | cut -c1-64); ";
    script += "[ -n \"$S\" ] || fail \"the host has neither shasum nor sha256sum to check the upload with\"; ";
    script += "[ \"$S\" = \"" + digest + "\" ] || fail \"the upload arrived damaged: its SHA-256 is $S, not " + digest + "\"; ";
    script += "chmod 700 \"$N\" || fail \"the upload could not be made executable\"; ";
    script += "V=$(\"$N\" --version 2>&1) || fail \"the uploaded host does not run on this machine: $V\"; ";
    script += "\"$N\" --help >/dev/null 2>&1 || fail \"the uploaded host failed its startup check\"; ";
    script += liveSessionCountFragment();
    switch (mode) {
    case HostInstallMode::IfIdle:
        script += "[ \"$L\" = 0 ] || fail \"$L sessions started on the host while it was being updated; nothing was replaced\"; ";
        break;
    case HostInstallMode::WhenSessionsEnd:
        script += "if [ \"$L\" != 0 ]; then mv -f \"$N\" \"$D/ubiquitty-host.pending\" || fail \"the new host could not be kept beside the running one\"; "
                  "printf \"\\n"
            + std::string(hostDeferredPrefix) + "%s\\n\" \"$L\"; exit 0; fi; ";
        break;
    case HostInstallMode::Now:
        break;
    }
    script += promoteHostFragment("\"$N\"");
    script += "rm -f \"$D/ubiquitty-host.pending\"; ";
    script += "T=$(\"$B\" --take-over 2>&1); R=$?; ";
    script += "if [ \"$R\" = " + std::to_string(hostHandoverPendingStatus) + " ]; then printf \"\\n" + std::string(hostHandoverPendingPrefix) + "%s\\n\" \"$T\"; exit 0; fi; ";
    script += "[ \"$R\" = 0 ] || fail \"the new host was installed but could not complete handover; existing sessions were not stopped: $T\"; ";
    script += "printf \"\\n" + std::string(hostInstalledPrefix) + " %s\\n\" \"$V\"";
    return script;
}

/// The bootstrap's first step: move a host an update left waiting into place
/// once the daemon it waited for holds no session.
///
/// Reads `$D`, `$B` and `$C`; refusals here change nothing and the bootstrap
/// goes on with the host already installed. The waiting host passed every
/// install check when it arrived.
///
/// - Returns: The fragment, ending in `; `.
inline std::string promotePendingHostFragment()
{
    return "if [ -x \"$D/ubiquitty-host.pending\" ]; then " + liveSessionCountFragment()
        + "if [ \"$L\" = 0 ]; then fail() { :; }; " + promoteHostFragment("\"$D/ubiquitty-host.pending\"") + "fi; fi; ";
}

/// How an install ended, from what it printed.
enum class HostInstallOutcomeKind : std::uint8_t {
    /// The new host is in place.
    Installed,
    /// The new host waits beside the running one for its sessions to end.
    Deferred,
    /// The new host is installed; a predecessor still serves its sessions.
    HandoverPending,
    /// The install or successor startup failed; no daemon was stopped.
    Failed,
};

/// How an install ended, and what a person is told.
struct HostInstallOutcome {
    HostInstallOutcomeKind kind { HostInstallOutcomeKind::Failed };
    /// For Deferred, the sessions the host waits on.
    std::size_t waitingSessionCount { 0 };
    /// For Failed or HandoverPending, why: the install's sentence, else
    /// the tail of what ssh and the remote shell printed.
    std::string reason;
};

/// Reads an install's output.
///
/// Success is the sentinel the script prints last, never ssh's exit status:
/// the sentinel proves every step before it ran.
///
/// - Parameters:
///   - output: Everything the install printed, standard error included.
///   - tail: The failure detail to fall back on when the script said nothing
///     of its own (``deployFailureDetail`` of the same output).
/// - Returns: The outcome.
inline HostInstallOutcome readHostInstallOutput(std::string_view output, std::string_view tail)
{
    const auto lineAfter = [&output](std::string_view prefix) -> std::optional<std::string_view> {
        const std::size_t at = output.rfind(prefix);
        if (at == std::string_view::npos || (at != 0 && output[at - 1] != '\n'))
            return std::nullopt;
        std::string_view rest = output.substr(at + prefix.size());
        rest = rest.substr(0, rest.find('\n'));
        if (!rest.empty() && rest.back() == '\r')
            rest.remove_suffix(1);
        return rest;
    };
    HostInstallOutcome outcome;
    if (const auto reason = lineAfter(hostInstallFailedPrefix)) {
        outcome.reason = reason->empty() ? std::string(tail) : std::string(*reason);
        return outcome;
    }
    if (const auto count = lineAfter(hostDeferredPrefix)) {
        std::size_t sessions = 0;
        bool isCount = !count->empty() && count->size() <= 8;
        for (const char digit : *count) {
            if (digit < '0' || digit > '9') {
                isCount = false;
                break;
            }
            sessions = sessions * 10 + static_cast<std::size_t>(digit - '0');
        }
        if (isCount) {
            outcome.kind = HostInstallOutcomeKind::Deferred;
            outcome.waitingSessionCount = sessions;
            return outcome;
        }
    }
    if (const auto reason = lineAfter(hostHandoverPendingPrefix)) {
        outcome.kind = HostInstallOutcomeKind::HandoverPending;
        outcome.reason = reason->empty() ? "The new host is installed, but an older daemon still holds sessions. They remain running." : std::string(*reason);
        return outcome;
    }
    if (lineAfter(hostInstalledPrefix)) {
        outcome.kind = HostInstallOutcomeKind::Installed;
        return outcome;
    }
    outcome.reason = std::string(tail);
    return outcome;
}

/// Whether an update must ask before it installs.
enum class HostUpdateConsent : std::uint8_t {
    /// The running daemon holds no session; nothing an install does ends one.
    Proceed,
    /// Sessions are running there; the person chooses live handover, waiting
    /// for them to end, or cancelling the update.
    AskFirst,
};

/// Decides whether an update asks before installing.
///
/// - Parameter liveSessionCount: What the probe counted.
/// - Returns: The decision.
[[nodiscard]] constexpr HostUpdateConsent decideHostUpdateConsent(std::size_t liveSessionCount)
{
    return liveSessionCount == 0 ? HostUpdateConsent::Proceed : HostUpdateConsent::AskFirst;
}

/// The question an update with live sessions asks, verbatim.
///
/// - Parameters:
///   - liveSessionCount: What the probe counted.
///   - host: What the person calls the host.
/// - Returns: The sentence.
inline std::string hostUpdateSessionsQuestion(std::size_t liveSessionCount, std::string_view host)
{
    const std::string where = host.empty() ? std::string("the host") : std::string(host);
    if (liveSessionCount == 1)
        return "1 session is running on " + where + ". Update now and keep it running?";
    return std::to_string(liveSessionCount) + " sessions are running on " + where + ". Update now and keep them running?";
}

/// What a deferred update tells the person.
///
/// - Parameters:
///   - waitingSessionCount: The sessions the new host waits on.
///   - host: What the person calls the host.
/// - Returns: The sentence.
inline std::string hostUpdateDeferredNotice(std::size_t waitingSessionCount, std::string_view host)
{
    const std::string where = host.empty() ? std::string("the host") : std::string(host);
    const std::string sessions = waitingSessionCount == 1 ? std::string("its session ends") : "its " + std::to_string(waitingSessionCount) + " sessions end";
    return "The new host is installed beside the one running on " + where + " and takes over on the first connect after " + sessions
        + ". Until then this Mac cannot attach there.";
}

} // namespace Ubiquitty::Session
