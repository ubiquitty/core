// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The remote shell checks ~/.ubiquitty/ubiquitty-host, reports a missing
// executable for installation, ensures the daemon, and announces the binary
// stream boundary before attaching. Pinned to one daemon's socket, it starts
// nothing and says when that daemon is gone (`instanceGoneSentinel`). It does
// not build from source. A host an
// update left waiting beside the running one (HostInstall.h) is moved into
// place first, once the daemon it waited for holds no session.

#include "Core/Session/HostInstall.h"
#include "Core/Session/HostInstanceInfo.h"
#include "Core/Transport/SSHHost.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Session {

// The line the bootstrap prints (to its stdout = the Sync transport pipe) when
// no usable host binary exists, so the client knows to
// push one. Keep in sync with the producer in remoteBootstrapCommand() below
// (both reference this single definition). The remote's `uname -sm` follows a
// space, then a newline.
inline constexpr char deploySentinel[] = "UBIQUITTY-NEED-DEPLOY";

/// Marks the end of shell output and the start of the Sync byte stream.
inline constexpr char syncSentinel[] = "UBIQUITTY-SYNC";

/// The line a bootstrap pinned to one daemon prints in place of the Sync
/// stream boundary when nothing listens on that daemon's socket: the daemon
/// has stopped, and asking it again gets the same answer. The socket's
/// basename follows a space, then a newline. A QUIC bootstrap prints the same
/// line, without the leading newline, in place of its dial line.
inline constexpr char instanceGoneSentinel[] = "UBIQUITTY-INSTANCE-GONE";

/// The status `ubiquitty-host --probe-daemon` exits with when nothing listens
/// on the socket it names. Every other status says nothing about the daemon,
/// so a bootstrap that reads one goes on to attach as it would have.
inline constexpr int daemonProbeGoneStatus = 3;

/// What one bootstrap output line says about a pinned daemon.
enum class InstanceGoneLine : std::uint8_t {
    /// Not a report about the daemon.
    Other,
    /// ``instanceGoneSentinel`` and a socket name: that daemon is gone.
    Gone,
    /// ``instanceGoneSentinel`` and something that is not a socket name.
    Malformed,
};

/// A bootstrap output line read for ``instanceGoneSentinel``.
struct InstanceGoneReading {
    InstanceGoneLine kind = InstanceGoneLine::Other;
    /// The gone daemon's socket name, inside the line read, for `Gone`.
    std::string_view socketName;
};

/// Reports whether a name is a daemon socket basename a bootstrap may carry:
/// letters, digits, `.`, `_` and `-`, so it holds no shell metacharacter and
/// no path separator.
///
/// - Parameter socketName: The basename.
/// - Returns: Whether it is one, at most 255 bytes.
[[nodiscard]] constexpr bool isDaemonSocketName(std::string_view socketName)
{
    if (socketName.empty() || socketName.size() > 255)
        return false;
    for (const char character : socketName) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
                || character == '.' || character == '_' || character == '-'))
            return false;
    }
    return true;
}

/// Reads one line of a bootstrap's output for a report that its pinned
/// daemon is gone. Every reader of bootstrap output, the stream's and the QUIC
/// bootstrap's alike, reads the line here.
///
/// - Parameter line: One line, without its newline; a carriage return
///   before the newline is ignored.
/// - Returns: What the line says, with the socket name it names.
[[nodiscard]] constexpr InstanceGoneReading readInstanceGoneLine(std::string_view line)
{
    if (line.ends_with('\r'))
        line.remove_suffix(1);
    constexpr std::string_view sentinel(instanceGoneSentinel);
    if (!line.starts_with(sentinel) || line.size() <= sentinel.size() || line[sentinel.size()] != ' ')
        return {};
    const std::string_view socketName = line.substr(sentinel.size() + 1);
    if (!isDaemonSocketName(socketName))
        return { .kind = InstanceGoneLine::Malformed, .socketName = {} };
    return { .kind = InstanceGoneLine::Gone, .socketName = socketName };
}

/// The shell fragment a bootstrap pinned to `$SOCK` runs before it announces
/// the Sync stream: `binary` is asked whether a daemon listens there, and when
/// nothing does the fragment prints ``instanceGoneSentinel`` and ends the
/// command. Any other answer, including a refusal from a host binary that has
/// no probe, lets the attach go ahead.
///
/// - Parameter binary: The host binary, a shell word.
/// - Returns: The fragment, ending in `; `.
inline std::string pinnedDaemonProbeFragment(std::string_view binary)
{
    return std::string(binary) + " --probe-daemon --socket \"$SOCK\" >/dev/null 2>&1; [ $? -ne " + std::to_string(daemonProbeGoneStatus)
        + " ] || { printf \"\\n" + std::string(instanceGoneSentinel) + " %s\\n\" \"$SOCK\"; exit 1; }; ";
}

// The literal token the bootstrap script carries so a reattach can pin a
// specific daemon socket. substituteReattachSocket() rewrites this exact text.
inline constexpr char reattachSocketToken[] = "SOCK=\"\";";

// The POSIX script, before Transport::remoteShellCommand makes it an SSH exec
// request. Daemon and executable substitutions precede that encoding.
//
// The script finishes a deferred update, then checks for an existing working
// binary; if missing, prints the deploy sentinel so the client auto-deploys.
// Otherwise ensures the daemon with QUIC and attaches, or, pinned, attaches to
// the pinned daemon when it still listens. No build logic — the host either
// has the binary or it doesn't.
inline std::string remoteBootstrapCommand()
{
    return hostInstallPathsFragment() + promotePendingHostFragment()
        + "SOCK=\"\"; "
          "{ [ -x \"$B\" ] && \"$B\" --help >/dev/null 2>&1; } || "
          "{ printf \"\\n"
        + std::string(deploySentinel)
        + " %s\\n\" \"$(uname -sm)\"; exit 1; }; "
          "if [ -z \"$SOCK\" ]; then \"$B\" --ensure-daemon --quic-port 0 || exit 1; else "
        + pinnedDaemonProbeFragment("\"$B\"")
        + "fi; printf \"\\n"
        + std::string(syncSentinel)
        + R"(\n"; if [ -n "$SOCK" ]; then exec "$B" --attach --socket "$SOCK"; else exec "$B" --attach; fi)";
}

/// Reaches an existing remote daemon without installing or starting one.
///
/// The exec channel carries control requests, so it announces the same Sync
/// stream boundary as an interactive connection. A missing binary or daemon
/// closes the request instead of changing the host to make it reachable; a
/// pinned daemon that is gone is reported as ``instanceGoneSentinel``.
///
/// - Returns: The command for an authenticated shell on the target host.
inline std::string passiveControlRemoteCommand()
{
    return "SOCK=\"\"; B=\"$HOME/.ubiquitty/ubiquitty-host\"; "
           "[ -x \"$B\" ] || exit 1; if [ -n \"$SOCK\" ]; then "
        + pinnedDaemonProbeFragment("\"$B\"") + "fi; printf \"\\n"
        + std::string(syncSentinel)
        + R"(\n"; if [ -n "$SOCK" ]; then exec "$B" --attach --socket "$SOCK"; else exec "$B" --attach; fi)";
}

// A LOCAL-daemon bridge carrying the same SOCK="" reattach token. `quotedBinary`
// is already shell-quoted by the caller.
// --quic-port 0 even on the stdio path so the daemon is always QUIC-capable
// (a non-QUIC daemon can't enable QUIC later, permanently blocking the QUIC bootstrap).
inline std::string localBridgeCommand(std::string_view quotedBinary)
{
    std::string binary(quotedBinary);
    return "SOCK=\"\"; if [ -z \"$SOCK\" ]; then " + binary
        + " --ensure-daemon --quic-port 0 || exit 1; else " + pinnedDaemonProbeFragment(binary) + "fi; if [ -n \"$SOCK\" ]; then exec "
        + binary + " --attach --socket \"$SOCK\"; else exec " + binary + " --attach; fi";
}

// Rewrite a bootstrap command to PIN a specific daemon socket for reattach,
// substituting reattachSocketToken (`SOCK="";`) for `SOCK="<socketName>";`.
// Returns unchanged when socketName is empty or the token is absent. First occurrence only.
inline std::string substituteReattachSocket(std::string_view command, std::string_view socketName)
{
    std::string result(command);
    if (result.empty() || socketName.empty())
        return result;
    const std::string_view token(reattachSocketToken);
    const std::size_t at = result.find(token);
    if (at == std::string::npos)
        return result;
    // socketName is characterSet-validated by isValidSelection ([A-Za-z0-9._-] only), so it
    // carries no shell metacharacters and is safe inside these double quotes; double
    // quotes also keep the command single-quote-free for the ssh single-quote wrap.
    std::string pinned = "SOCK=\"";
    pinned += std::string(socketName);
    pinned += "\";";
    result.replace(at, token.size(), pinned);
    return result;
}

/// Drops only the exact validated predecessor pin from a bootstrap command.
///
/// A planned handover rediscovers the successor using the original bootstrap;
/// a different pin or a command without this token is never rewritten.
/// - Parameters:
///   - command: A bootstrap command pinned by substituteReattachSocket.
///   - socketName: The exact predecessor basename pinned by the factory.
/// - Returns: The command with its first exact pin restored to the empty token.
inline std::string clearReattachSocket(std::string_view command, std::string_view socketName)
{
    std::string result(command);
    if (socketName.empty())
        return result;
    const std::string pin = substituteReattachSocket(reattachSocketToken, socketName);
    const auto at = result.find(pin);
    if (at != std::string::npos)
        result.replace(at, pin.size(), reattachSocketToken);
    return result;
}

/// The literal token a QUIC bootstrap command carries so the same command can
/// be run as a probe of where ssh connects. `substituteReachProbe` rewrites
/// this exact text.
inline constexpr char reachProbeToken[] = "REACH_PROBE=\"\";";

/// Rewrites a QUIC bootstrap command to run its probe instead: one ssh
/// connection of its own to the same destination, sharing no other ssh's
/// connection. Its transcript says where ssh connects when the bootstrap's
/// own run went over a shared connection (ControlMaster) and so said nothing
/// (`SSHReachKind::Multiplexed`).
///
/// The probe never logs in. The caller ends it as soon as its transcript
/// names the connection, which comes before the key exchange; and it is run
/// knowing no host key and requiring one, so a probe that was not ended in
/// time stops at the key exchange. What the host sees is one TCP connection
/// closed before authentication: nothing an intrusion filter counts as a
/// failed login, whatever credentials the person's own ssh would need.
///
/// - Parameter command: The bootstrap command.
/// - Returns: The probe, or the command unchanged when it carries no token:
///   a local plan's, which runs no ssh.
inline std::string substituteReachProbe(std::string_view command)
{
    std::string result(command);
    const std::string_view token(reachProbeToken);
    const std::size_t at = result.find(token);
    if (at != std::string::npos)
        result.replace(at, token.size(), "REACH_PROBE=\"1\";");
    return result;
}

/// Everything a remote session needs to reach one host, derived from the
/// host alone.
///
/// The transport a session can reach must not depend on which door the user
/// came through. Spelled at each call site it did: the connect sheet filled
/// the QUIC pair, and the paths that inherit a remote identity or restore one
/// carried only the shell command, so those windows could never dial QUIC
/// however healthy the host was. Derived here, every path that knows the host
/// gets the same plan.
struct RemoteTransportPlan {
    /// The stdio transport command — the carrier that always works.
    std::string shellCommand;
    /// The host a QUIC carrier is offered for, empty when QUIC is not
    /// offered. A loopback address is dialled as it stands. Any other names
    /// the host and is never resolved: the carrier dials the address the
    /// bootstrap's ssh run reached (`SSHReach.h`).
    std::string quicHost;
    /// The command printing the daemon's QUIC port, one-time token and
    /// certificate pin, empty likewise. Over ssh it runs under `-v`, whose
    /// transcript on standard error says where ssh connected, and it carries
    /// `reachProbeToken`.
    std::string quicBootstrapCommand;
};

/// The command a REMOTE shell runs to print the daemon's QUIC bootstrap line.
///
/// The inner half of remoteTransportPlan's quicBootstrapCommand, named once so
/// the two callers cannot drift. The ssh plan below wraps it in `ssh <host>
/// '...'` and pays for a local subprocess; a frontend that already holds an
/// authenticated channel to the host sends this command over that channel
/// instead of wrapping it in ssh.
///
/// That distinction is not a nicety. iOS is sandboxed and cannot fork an ssh,
/// so the wrapped form is unreachable there — the phone has a live SSH carrier
/// and an exec session is the only way it can ask the daemon anything.
///
/// - Returns: The shell command, for a shell already on the target host.
inline std::string quicBootstrapRemoteCommand()
{
    return "SOCK=\"\"; B=\"$HOME/.ubiquitty/ubiquitty-host\"; if [ -n \"$SOCK\" ]; then exec \"$B\" --quic-bootstrap --socket \"$SOCK\"; "
           "else exec \"$B\" --quic-bootstrap; fi";
}

/// The plan for a session on this machine, through the local daemon.
///
/// - Parameter quotedBinary: The host binary's path, already shell-quoted.
/// - Returns: The stdio command and the QUIC pair.
inline RemoteTransportPlan localTransportPlan(std::string_view quotedBinary)
{
    const std::string binary(quotedBinary);
    return RemoteTransportPlan {
        .shellCommand = localBridgeCommand(quotedBinary),
        // The loopback address rather than the name: a QUIC carrier dials an
        // address, and the daemon it reaches is this machine's.
        .quicHost = "127.0.0.1",
        .quicBootstrapCommand = "SOCK=\"\"; if [ -n \"$SOCK\" ]; then exec " + binary + " --quic-bootstrap --socket \"$SOCK\"; else exec " + binary + " --quic-bootstrap; fi",
    };
}

/// Reports whether a host is this machine by its loopback address, which a
/// carrier dials as it stands: nothing was resolved to name it.
///
/// - Parameter host: The plan's QUIC host.
/// - Returns: Whether it is the IPv4 or IPv6 loopback address.
[[nodiscard]] inline bool isLoopbackAddress(std::string_view host)
{
    return host == "127.0.0.1" || host == "::1";
}

namespace Detail {

/// Splits a command led by the reattach token, pinned to a socket or not.
///
/// - Parameter command: A carrier command.
/// - Returns: The socket name, empty when unpinned, and the command after the
///   token; nothing when the command does not start with it.
[[nodiscard]] inline std::optional<std::pair<std::string_view, std::string_view>> splitReattachSocket(std::string_view command)
{
    constexpr std::string_view head = "SOCK=\"";
    constexpr std::string_view close = "\"; ";
    if (!command.starts_with(head))
        return std::nullopt;
    const std::size_t end = command.find(close, head.size());
    if (end == std::string_view::npos)
        return std::nullopt;
    const std::string_view socket = command.substr(head.size(), end - head.size());
    const bool isSocketName = std::ranges::all_of(socket, [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
    });
    if (!isSocketName)
        return std::nullopt;
    return std::pair { socket, command.substr(end + close.size()) };
}

/// Whether a word is one shell-quoted word as `shellSingleQuote` writes it.
///
/// - Parameter word: The candidate.
/// - Returns: Whether quoting the word's unquoted text gives the word back.
[[nodiscard]] inline bool isShellSingleQuotedWord(std::string_view word)
{
    if (word.size() < 2 || word.front() != '\'' || word.back() != '\'')
        return false;
    std::string unquoted;
    const std::string_view inner = word.substr(1, word.size() - 2);
    constexpr std::string_view escapedQuote = "'\\''";
    for (std::size_t index = 0; index < inner.size();) {
        if (inner.substr(index).starts_with(escapedQuote)) {
            unquoted.push_back('\'');
            index += escapedQuote.size();
        } else {
            unquoted.push_back(inner[index]);
            ++index;
        }
    }
    return Transport::shellSingleQuote(unquoted) == word;
}

} // namespace Detail

/// Whether a carrier's commands are ``localTransportPlan``'s, pinned to a
/// daemon socket or not: the stream bridge to this machine's own daemon, and
/// either no QUIC carrier or one on the loopback address.
///
/// The client's own commands are the evidence, never the host's word. Any
/// other command may run anything, `ssh` included, so it places the host on
/// another machine. The bridge and the bootstrap may name different binaries
/// on this machine: a restored tab keeps the bridge it was saved with and
/// takes the bootstrap of the binary the app now carries.
///
/// - Parameters:
///   - shellCommand: The stream carrier's command.
///   - quicHost: The QUIC carrier's host, empty when QUIC is not offered.
///   - quicBootstrapCommand: The QUIC bootstrap's command.
/// - Returns: Whether every carrier the commands can build reaches this machine.
[[nodiscard]] inline bool isLocalTransportPlan(std::string_view shellCommand, std::string_view quicHost, std::string_view quicBootstrapCommand)
{
    // The one shell-quoted binary between `prefix` and `suffix`, if `command`
    // is `expected(binary)` pinned to its own socket or not.
    const auto isOneBinaryCommand = [](std::string_view command, std::string_view prefix, std::string_view suffix, auto expected) {
        const auto split = Detail::splitReattachSocket(command);
        if (!split || !split->second.starts_with(prefix))
            return false;
        const std::string_view rest = split->second.substr(prefix.size());
        const std::size_t binaryEnd = rest.find(suffix);
        if (binaryEnd == std::string_view::npos)
            return false;
        const std::string_view binary = rest.substr(0, binaryEnd);
        return Detail::isShellSingleQuotedWord(binary) && substituteReattachSocket(expected(binary), split->first) == command;
    };
    if (!isOneBinaryCommand(shellCommand, "if [ -z \"$SOCK\" ]; then ", " --ensure-daemon ", [](std::string_view binary) { return localBridgeCommand(binary); }))
        return false;
    if (quicHost.empty())
        return true;
    return isLoopbackAddress(quicHost)
        && isOneBinaryCommand(quicBootstrapCommand, "if [ -n \"$SOCK\" ]; then exec ", " --quic-bootstrap ", [](std::string_view binary) { return localTransportPlan(binary).quicBootstrapCommand; });
}

/// What a QUIC bootstrap run that printed no usable line says about its host.
enum class QUICBootstrapMiss : uint8_t {
    /// The host was reached and offered no QUIC: no daemon binary, a refused
    /// login, or a line that did not parse. Asking again gets the same answer.
    Declined,
    /// The host was never reached, so it has declined nothing and the next
    /// attempt may reach it.
    Unreached,
};

/// How long ssh(1) has to reach a host, in seconds. ssh applies it to the TCP
/// connect and to the banner exchange that follows.
inline constexpr int sshConnectTimeoutSeconds = 10;

/// How long a QUIC bootstrap run has to print its line, in seconds.
///
/// Longer than ``sshConnectTimeoutSeconds``, so a run still going at this
/// deadline has heard the host's banner: what is slow is the login or the
/// remote daemon's start, and either is the host's answer.
inline constexpr int quicBootstrapDeadlineSeconds = 15;
static_assert(quicBootstrapDeadlineSeconds > sshConnectTimeoutSeconds,
    "a bootstrap abandoned at its deadline must already be past ssh's connect");

/// Reads why a QUIC bootstrap printed no usable line.
///
/// ssh(1) reports a connect that never reached the host on its standard error
/// with fixed prefixes, the same way `handleClose` reads its authentication
/// failures. A refused connection is the host's answer, as
/// ``Transport::isTransientConnectFailure`` reads it: the machine is up and
/// nothing listens on the port. A refused login is the host's answer too. A run
/// abandoned at its deadline reports nothing here, because ssh says within its
/// connect timeout when it cannot reach a host, and that timeout ends before
/// the deadline does.
///
/// - Parameter standardError: What the run wrote to its standard error,
///   possibly cut short.
/// - Returns: Whether the host declined or was never reached.
[[nodiscard]] inline QUICBootstrapMiss classifyQUICBootstrapMiss(std::string_view standardError)
{
    constexpr std::string_view reachFailures[] = {
        "ssh: connect to host ",
        "ssh: Could not resolve hostname ",
        "kex_exchange_identification: ",
        "Connection timed out during banner exchange",
    };
    // Only a line that begins with one: the bootstrap runs under `-v`, whose
    // diagnostics carry the same words mid-line for a host that was reached
    // (`debug1: kex_exchange_identification: banner line 0: …`, or a refused
    // first address ahead of one that answered).
    std::size_t at = 0;
    while (at < standardError.size()) {
        const auto end = standardError.find('\n', at);
        const auto line = standardError.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? standardError.size() : end + 1;
        for (const auto failure : reachFailures) {
            if (!line.starts_with(failure))
                continue;
            if (line.find("Connection refused") != std::string_view::npos)
                return QUICBootstrapMiss::Declined;
            return QUICBootstrapMiss::Unreached;
        }
    }
    return QUICBootstrapMiss::Declined;
}

/// The plan for a session on `sshHost`, through its daemon over ssh.
///
/// - Parameters:
///   - sshHost: The validated `[user@]host`.
/// - Returns: The stdio command and the QUIC pair.
inline RemoteTransportPlan remoteTransportPlan(std::string_view sshHost)
{
    const std::string host(sshHost);
    // An unreachable peer must return control to the session's retry state.
    const std::string sshCommand = "ssh -o BatchMode=yes -o ConnectTimeout=" + std::to_string(sshConnectTimeoutSeconds)
        + " -o ServerAliveInterval=15 -o ServerAliveCountMax=3 ";
    const std::string quotedHost = Ubiquitty::Transport::shellSingleQuote(host);
    return RemoteTransportPlan {
        // `exec ssh` so a detach's SIGTERM reaches ssh directly: the channel
        // closes, the remote --attach sees EOF and detaches, and the daemon
        // and its sessions live on.
        .shellCommand = "exec " + sshCommand + quotedHost + " " + Transport::shellSingleQuote(Transport::remoteShellCommand(remoteBootstrapCommand())),
        .quicHost = host.substr(host.rfind('@') == std::string::npos ? 0 : host.rfind('@') + 1),
        // The remote binary's own bootstrap: it ensures a QUIC-capable daemon
        // and prints the port, the token and the pin. `-v` has ssh say on
        // standard error which address it connected to, which is where the
        // carrier dials; nothing about how ssh dials is changed to learn it.
        // The probe is the same connect with no shared connection, for a
        // run that went over another ssh's: see `substituteReachProbe`.
        .quicBootstrapCommand = std::string(reachProbeToken) + " if [ -n \"$REACH_PROBE\" ]; then exec " + sshCommand
            + "-v -o ControlMaster=no -o ControlPath=none -o StrictHostKeyChecking=yes -o UserKnownHostsFile=/dev/null "
              "-o GlobalKnownHostsFile=/dev/null -o VerifyHostKeyDNS=no "
            + quotedHost + " " + Transport::shellSingleQuote(Transport::remoteShellCommand("true")) + "; fi; "
            + sshCommand + "-v " + quotedHost + " " + Transport::shellSingleQuote(Transport::remoteShellCommand(quicBootstrapRemoteCommand())),
    };
}

/// Redirects an owner-bound bootstrap to the executable that serves that owner.
/// The existing socket pin stays intact, including an explicit successor reset.
[[nodiscard]] inline std::string hostInstanceBootstrap(std::string_view command, const HostInstanceInfo& instance)
{
    if (!isValidHostInstance(instance))
        return {};
    constexpr std::string_view installed = "\"$HOME/.ubiquitty/ubiquitty-host\"";
    std::string result(command);
    const auto position = result.find(installed);
    if (position == std::string::npos)
        return result.find(Transport::shellSingleQuote(instance.executable)) != std::string::npos ? result : std::string {};
    result.replace(position, installed.size(), Transport::shellSingleQuote(instance.executable));
    return result;
}

/// Removes the reattach token a command opens its socket choice with, so the
/// command can run under another command that carries the token.
///
/// - Parameter command: A command carrying ``reattachSocketToken``.
/// - Returns: The command with its first token, and the space after it, gone.
inline std::string withoutReattachSocketToken(std::string_view command)
{
    std::string result(command);
    const std::string token = std::string(reattachSocketToken) + ' ';
    if (const auto at = result.find(token); at != std::string::npos)
        result.erase(at, token.size());
    return result;
}

/// Reaches one discovered daemon through its own executable and socket, and
/// the host's current daemon once the socket pin is withdrawn.
///
/// Pinned, each command reaches that daemon with its executable and starts
/// nothing: the stream probes it and attaches, the QUIC bootstrap asks it. With
/// the pin cleared (``clearReattachSocket``), as a session clears it for a
/// daemon reported gone, each command is the host's own plan, which ensures the
/// host's current daemon and reaches it. A window whose picked daemon stopped is
/// never attached to the default socket of an executable nothing started.
///
/// - Parameters:
///   - sshHost: The validated `[user@]host`, empty for this machine.
///   - instance: The discovered daemon.
///   - quotedLocalBinary: This machine's host binary, shell-quoted: the
///     local plan's. Read only when `sshHost` is empty.
/// - Returns: The plan, empty for an invalid instance or host.
[[nodiscard]] inline RemoteTransportPlan hostInstanceTransportPlan(std::string_view sshHost, const HostInstanceInfo& instance, std::string_view quotedLocalBinary)
{
    if (!isValidHostInstance(instance) || (!sshHost.empty() && !Transport::validateSSHHost(sshHost)) || (sshHost.empty() && quotedLocalBinary.empty()))
        return {};
    const std::string executable = Transport::shellSingleQuote(instance.executable);
    const std::string token = std::string(reattachSocketToken) + " ";
    const std::string pinnedQUIC = "if [ -n \"$SOCK\" ]; then exec " + executable + " --quic-bootstrap --socket \"$SOCK\"; fi; ";
    if (sshHost.empty()) {
        auto plan = localTransportPlan(quotedLocalBinary);
        const std::string pinnedAttach = "if [ -n \"$SOCK\" ]; then " + pinnedDaemonProbeFragment(executable) + "exec " + executable + " --attach --socket \"$SOCK\"; fi; ";
        plan.shellCommand = substituteReattachSocket(token + pinnedAttach + withoutReattachSocketToken(plan.shellCommand), instance.socketName);
        plan.quicBootstrapCommand = substituteReattachSocket(token + pinnedQUIC + withoutReattachSocketToken(plan.quicBootstrapCommand), instance.socketName);
        return plan;
    }
    auto plan = remoteTransportPlan(sshHost);
    const auto replaceCommand = [](std::string& outer, const std::string& original, const std::string& replacement) {
        const auto quoted = Transport::shellSingleQuote(Transport::remoteShellCommand(original));
        const auto at = outer.find(quoted);
        if (at != std::string::npos)
            outer.replace(at, quoted.size(), Transport::shellSingleQuote(Transport::remoteShellCommand(replacement)));
    };
    const std::string pinnedAttach = "if [ -n \"$SOCK\" ]; then [ -x " + executable + " ] || exit 1; " + pinnedDaemonProbeFragment(executable)
        + "printf \"\\n" + std::string(syncSentinel) + "\\n\"; exec " + executable + " --attach --socket \"$SOCK\"; fi; ";
    replaceCommand(plan.shellCommand, remoteBootstrapCommand(),
        substituteReattachSocket(token + pinnedAttach + withoutReattachSocketToken(remoteBootstrapCommand()), instance.socketName));
    replaceCommand(plan.quicBootstrapCommand, quicBootstrapRemoteCommand(),
        substituteReattachSocket(token + pinnedQUIC + withoutReattachSocketToken(quicBootstrapRemoteCommand()), instance.socketName));
    return plan;
}

/// What a window opened from a host's listing asks the host for.
enum class ListedOpen : std::uint8_t {
    /// A session or workspace that does not exist yet.
    Creates,
    /// A session, a workspace or a workspace record the listing showed.
    OpensListed,
};

/// The daemon a window dials.
struct DaemonPin {
    /// The socket basename the window pins, empty for the host's current
    /// daemon.
    std::string socketName;
    /// The daemon instance the window expects, 0 for whichever answers.
    std::uint64_t daemonInstanceID = 0;
};

/// Decides which daemon a window opened from a host's listing dials.
///
/// What the listing showed is held by the daemon that listed it, so opening it
/// pins that daemon, and a reattach by session id also expects that daemon's
/// instance. A window that creates a session or workspace dials the host's
/// current daemon whichever daemon answered the listing: a daemon this app
/// remembers may have stopped since, and a new session belongs on the one the
/// host runs now.
///
/// - Parameters:
///   - open: What the window asks for.
///   - reattachSessionID: The session it reattaches to, 0 for none.
///   - listedSocketName: The socket of the daemon that answered the listing.
///   - listedDaemonInstanceID: That daemon's instance.
/// - Returns: The pin, empty for the host's current daemon.
[[nodiscard]] inline DaemonPin pinForListedOpen(ListedOpen open, std::uint32_t reattachSessionID, std::string_view listedSocketName, std::uint64_t listedDaemonInstanceID)
{
    if (open == ListedOpen::Creates || !isDaemonSocketName(listedSocketName))
        return {};
    return { .socketName = std::string(listedSocketName), .daemonInstanceID = reattachSessionID ? listedDaemonInstanceID : 0 };
}

/// Decides whether the daemon a person picked to list has gone, so the
/// picker gives way to the host's current daemon.
///
/// Only a probe of the picked socket establishes that it is gone. A different
/// listing may be stale or misrouted and says nothing about that socket's lifetime.
///
/// - Parameters:
///   - pickedSocketName: The picked daemon's socket, empty for none picked.
///   - goneSocketName: The socket a bootstrap last reported gone, empty for none.
///   - answeredSocketName: The socket of the daemon the last listing came
///     from, empty when it was not answered.
/// - Returns: Whether the pick is to be dropped.
[[nodiscard]] constexpr bool isPickedDaemonGone(std::string_view pickedSocketName, std::string_view goneSocketName, std::string_view answeredSocketName)
{
    if (pickedSocketName.empty())
        return false;
    (void)answeredSocketName;
    return goneSocketName == pickedSocketName;
}

} // namespace Ubiquitty::Session
